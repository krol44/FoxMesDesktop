/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_search_adapter.h"

#include "base/flat_map.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_msg_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_session.h"

#include <algorithm>
#include <memory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace CustomBackend::Search {
namespace {

// Upstream stores the request id to guard against a second request in flight,
// to tell its own answer from a superseded one and to cancel it. There is no
// MTProto request under the bridge, so a marker stands in for one - the same
// trick HistoryWidget uses for history pages and PinMessageBox for a pin in
// flight. It has to be unique per call, because a single shared value would
// make every stale answer look current; negative values never collide with a
// real mtpRequestId, and cancelling an unknown one is a no-op.
[[nodiscard]] mtpRequestId NextRequestMarker() {
	static auto counter = mtpRequestId(0);
	return --counter;
}

constexpr auto kSearchPerPage = 40;

struct MessagesSearchState {
	bool pending = false;
	bool exhausted = false;
	int generation = 0;
	int total = 0;
	qint64 nextBefore = 0;
	QString query;
	std::shared_ptr<char> alive = std::make_shared<char>();
};

base::flat_map<const void*, MessagesSearchState> &MessagesSearchStates() {
	static auto result = base::flat_map<const void*, MessagesSearchState>();
	return result;
}

} // namespace

void SearchPeers(
		not_null<Main::Session*> session,
		const QString &query,
		Fn<void(Api::PeerSearchResult)> done) {
	ClientFor(session).users(query, [=](QJsonDocument doc, QString error, int) {
		auto parsed = Api::PeerSearchResult();
		if (error.isEmpty() && doc.isArray()) {
			for (const auto &entry : doc.array()) {
				if (!entry.isObject()) continue;
				const auto object = entry.toObject();
				const auto id = object.value("id").toVariant().toLongLong();
				if (id <= 0) continue;
				const auto user = session->data().user(UserId(id));
				if (const auto bridge = BridgeFor(session)) {
					// The same applier every other peer goes through, so a
					// person found by search arrives with the avatar the DTO
					// carries instead of initials.
					//
					// Not as a contact: a search hit is somebody the account
					// has never talked to, and upstream lists a contact
					// without a chat in the local half of the search - which
					// showed the same person twice, once above the global
					// results and once in them.
					bridge->ensureUser(object, false);
				} else {
					user->setName(
						object.value("display_name").toString(),
						QString(),
						QString(),
						object.value("username").toString());
					// Upstream drops updates for a peer it does not consider
					// loaded.
					if (!user->isLoaded()) {
						user->setLoadedStatus(PeerData::LoadedStatus::Normal);
					}
				}
				parsed.peers.push_back(user);
			}
		}
		done(std::move(parsed));
	});
}

mtpRequestId SearchPeersFound(
		not_null<Main::Session*> session,
		const QString &query,
		Fn<void(MTPcontacts_Found, mtpRequestId)> done) {
	const auto marker = NextRequestMarker();
	SearchPeers(session, query, [marker, done = std::move(done)](
			Api::PeerSearchResult parsed) {
		auto results = QVector<MTPPeer>();
		results.reserve(parsed.peers.size());
		for (const auto &peer : parsed.peers) {
			results.push_back(peerToMTP(peer->id));
		}
		done(MTP_contacts_found(
			MTP_vector<MTPPeer>(),
			MTP_vector<MTPPeer>(std::move(results)),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()),
			marker);
	});
	return marker;
}

mtpRequestId SearchInChats(
		not_null<Main::Session*> session,
		const QString &query,
		PeerData *inPeer,
		qint64 before,
		Fn<void(MTPmessages_Messages, mtpRequestId)> done) {
	const auto marker = NextRequestMarker();
	const auto bridge = BridgeFor(session);
	// Deferred on purpose: upstream assigns the request id from the return
	// value of this call, so a completion that runs before the assignment
	// would leave the marker behind and block every later search.
	const auto reportEmpty = [done, marker] {
		crl::on_main([done, marker] {
			done(MTP_messages_messages(
				MTP_vector<MTPMessage>(),
				MTP_vector<MTPForumTopic>(),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>()),
				marker);
		});
	};
	if (!bridge) {
		reportEmpty();
		return marker;
	}
	const auto history = inPeer ? session->data().historyLoaded(inPeer) : nullptr;
	if (inPeer && !history) {
		reportEmpty();
		return marker;
	}
	bridge->searchAllChats(query, history, before, kSearchPerPage, [
		marker,
		done = std::move(done)
	](NativeBridge::SearchPage page) {
		if (page.messages.isEmpty()) {
			// An empty slice would leave next_rate unchanged, which upstream
			// reads as "not finished" for a global search; the plain
			// messages.messages shape is what marks the search complete.
			done(MTP_messages_messages(
				MTP_vector<MTPMessage>(),
				MTP_vector<MTPForumTopic>(),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>()),
				marker);
			return;
		}
		using Flag = MTPDmessages_messagesSlice::Flag;
		done(MTP_messages_messagesSlice(
			MTP_flags(Flag::f_next_rate),
			MTP_int(page.total),
			// next_rate is what a global search pages by upstream: it must
			// change while more pages exist and stop changing when they do
			// not, which is exactly what the oldest id of the page does.
			MTP_int(int(page.hasMore ? page.nextBefore : 0)),
			MTPint(), // offset_id_offset
			MTPSearchPostsFlood(),
			MTP_vector<MTPMessage>(std::move(page.messages)),
			MTP_vector<MTPForumTopic>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()),
			marker);
	});
	return marker;
}

void RequestMessages(
		not_null<const void*> owner,
		not_null<History*> history,
		const QString &query,
		const QString &nextToken,
		Fn<void(Api::FoundMessages)> done) {
	if (query.trimmed().isEmpty()) {
		done({ 0, {}, nextToken });
		return;
	}
	auto &state = MessagesSearchStates()[owner];
	// A new (or changed) query restarts pagination; the same query continues
	// from the stored server cursor.
	if (state.query != query) {
		state.query = query;
		state.nextBefore = 0;
		state.total = 0;
		state.exhausted = false;
	}
	if (state.exhausted) {
		// Pagination already complete: answer without a network round trip so
		// upstream searchMore() loops terminate. The total stays the server
		// one, because upstream keeps reading it from every page.
		done({ state.total, {}, nextToken });
		return;
	}
	const auto before = state.nextBefore;
	state.pending = true;
	const auto generation = ++state.generation;
	const auto alive = std::weak_ptr<char>(state.alive);
	const auto bridge = BridgeFor(&history->session());
	if (!bridge) {
		state.pending = false;
		done({ 0, {}, nextToken });
		return;
	}
	bridge->searchMessages(history, query, before, 40, [
		alive,
		owner,
		history,
		generation,
		nextToken,
		done = std::move(done)
	](std::vector<int32_t> ids, bool hasMore, int total) mutable {
		if (alive.expired()) {
			return;
		}
		auto &states = MessagesSearchStates();
		const auto i = states.find(owner);
		if (i == end(states)) {
			return;
		}
		auto &state = i->second;
		if (generation != state.generation) {
			// Stale response: a newer request superseded this one.
			return;
		}
		state.pending = false;
		if (!hasMore || ids.empty()) {
			state.exhausted = true;
		}
		if (!ids.empty()) {
			// Continue from the oldest id of this page.
			state.nextBefore = ids.back();
		}
		auto found = MessageIdsList();
		found.reserve(ids.size());
		for (const auto id : ids) {
			found.push_back(FullMsgId(history->peer->id, MsgId(id)));
		}
		// The server total, not the page size: MessagesSearchMerged treats
		// "total == loaded" as the end of the search, so reporting the page
		// size stops paging after the first page and freezes the "N of M"
		// counter at it.
		state.total = std::max(state.total, total);
		done({ state.total, std::move(found), nextToken });
	});
}

bool MessagesPending(not_null<const void*> owner) {
	const auto &states = MessagesSearchStates();
	const auto i = states.find(owner);
	return (i != end(states)) && i->second.pending;
}

void CancelMessages(not_null<const void*> owner) {
	MessagesSearchStates().remove(owner);
}

} // namespace CustomBackend::Search
