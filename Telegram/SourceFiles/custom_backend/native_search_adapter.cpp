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
					bridge->ensureUser(object, false);
				} else {
					user->setName(
						object.value("display_name").toString(),
						QString(),
						QString(),
						object.value("username").toString());
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
			MTP_int(int(page.hasMore ? page.nextBefore : 0)),
			MTPint(),
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
	if (state.query != query) {
		state.query = query;
		state.nextBefore = 0;
		state.total = 0;
		state.exhausted = false;
	}
	if (state.exhausted) {
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
			return;
		}
		state.pending = false;
		if (!hasMore || ids.empty()) {
			state.exhausted = true;
		}
		if (!ids.empty()) {
			state.nextBefore = ids.back();
		}
		auto found = MessageIdsList();
		found.reserve(ids.size());
		for (const auto id : ids) {
			found.push_back(FullMsgId(history->peer->id, MsgId(id)));
		}
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

}
