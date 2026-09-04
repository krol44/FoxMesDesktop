/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_shared_media_adapter.h"

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/storage_facade.h"

namespace CustomBackend::SharedMedia {
namespace {

using Type = Storage::SharedMediaType;

// One page of a media list. The server splits it in half around an anchor,
// the same way a history page around a position is split.
constexpr auto kMediaLimit = 40;

// Info::GlobalMedia::Provider stores the request id, compares a completion
// against it and cancels it on teardown. There is no MTProto request under
// the bridge, so a marker stands in for one; cancelling an unknown id is a
// no-op, which is exactly what upstream needs it to be.
constexpr auto kBridgeGlobalMarker = mtpRequestId(-1);

struct RequestKey {
	PeerId peerId = 0;
	Type type = Type::kCount;
	MsgId messageId = 0;
	Data::LoadDirection direction = Data::LoadDirection::Around;

	friend inline auto operator<=>(
		const RequestKey&,
		const RequestKey&) = default;
	friend inline bool operator==(
		const RequestKey&,
		const RequestKey&) = default;
};

// Mirrors ApiWrap::_sharedMediaRequests: the slice builder keeps asking for
// the same gap until it is filled, so without this the same page would be
// requested on every snapshot.
base::flat_set<RequestKey> &PendingRequests() {
	static auto result = base::flat_set<RequestKey>();
	return result;
}

// The in-flight requests of one list, by their own id. A generation counter
// would be wrong here: the slice builder can ask for two gaps at once, and a
// later request must not silently drop the answer to the earlier one - the
// caller keeps that gap marked as requested and would never fill it.
struct SearchState {
	base::flat_set<int> live;
	int lastId = 0;
};

base::flat_map<const void*, SearchState> &SearchStates() {
	static auto result = base::flat_map<const void*, SearchState>();
	return result;
}

[[nodiscard]] QString KindFor(Type type) {
	switch (type) {
	case Type::Photo: return u"photo"_q;
	case Type::Video: return u"video"_q;
	case Type::PhotoVideo: return u"photo_video"_q;
	case Type::File: return u"file"_q;
	case Type::MusicFile: return u"music"_q;
	case Type::RoundVoiceFile: return u"voice"_q;
	case Type::GIF: return u"gif"_q;
	case Type::Link: return u"link"_q;
	default: return QString();
	}
}

struct Anchors {
	qint64 before = 0;
	qint64 after = 0;
	qint64 around = 0;
};

[[nodiscard]] Anchors AnchorsFor(
		MsgId messageId,
		Data::LoadDirection direction) {
	if (!messageId) {
		return {};
	}
	switch (direction) {
	case Data::LoadDirection::Before: return { .before = messageId.bare };
	case Data::LoadDirection::After: return { .after = messageId.bare };
	case Data::LoadDirection::Around: return { .around = messageId.bare };
	}
	Unexpected("Direction in CustomBackend::SharedMedia::AnchorsFor");
}

// Builds the parsed page the way Api::ParseSearchResult does: the client
// decides the media type of an item, so a page is filtered by what the built
// item actually is, and the range is widened to the edge the server reported
// as exhausted.
[[nodiscard]] Api::SearchResult ResultFrom(
		not_null<PeerData*> peer,
		Type type,
		MsgId messageId,
		Data::LoadDirection direction,
		const NativeBridge::MediaPage &page) {
	auto result = Api::SearchResult();
	result.noSkipRange = MsgRange{ messageId, messageId };
	result.fullCount = page.total;
	result.messageIds.reserve(page.ids.size());
	for (const auto id : page.ids) {
		const auto itemId = MsgId(id);
		const auto item = peer->owner().message(peer->id, itemId);
		if (!item) {
			continue;
		}
		if ((type == Type::kCount) || item->sharedMediaTypes().test(type)) {
			result.messageIds.push_back(itemId);
		}
		accumulate_min(result.noSkipRange.from, itemId);
		accumulate_max(result.noSkipRange.till, itemId);
	}
	if (!page.hasMoreBefore) {
		result.noSkipRange.from = 0;
	}
	if (!page.hasMoreAfter) {
		result.noSkipRange.till = ServerMaxMsgId;
	}
	if (messageId && result.messageIds.empty()) {
		switch (direction) {
		case Data::LoadDirection::Before:
			result.noSkipRange.from = 0;
			break;
		case Data::LoadDirection::Around:
			result.noSkipRange = { 0, ServerMaxMsgId };
			break;
		case Data::LoadDirection::After:
			result.noSkipRange.till = ServerMaxMsgId;
			break;
		}
	}
	return result;
}

} // namespace

bool Supported(Type type) {
	return !KindFor(type).isEmpty();
}

void Request(
		Main::Session *session,
		PeerData *peer,
		Type type,
		MsgId messageId,
		Data::LoadDirection direction) {
	const auto kind = KindFor(type);
	if (!session || !peer || kind.isEmpty()) {
		return;
	}
	const auto bridge = BridgeFor(session);
	const auto history = session->data().historyLoaded(peer);
	if (!bridge || !history) {
		return;
	}
	const auto key = RequestKey{
		.peerId = peer->id,
		.type = type,
		.messageId = messageId,
		.direction = direction,
	};
	if (!PendingRequests().emplace(key).second) {
		return;
	}
	const auto anchors = AnchorsFor(messageId, direction);
	const auto weak = base::make_weak(session);
	bridge->requestChatMedia(
		history,
		kind,
		QString(),
		anchors.before,
		anchors.after,
		anchors.around,
		kMediaLimit,
		[=](NativeBridge::MediaPage page) {
			PendingRequests().remove(key);
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto peer = strong->data().peerLoaded(key.peerId);
			if (!peer) {
				return;
			}
			auto parsed = ResultFrom(peer, type, messageId, direction, page);
			// An empty page of an exhausted direction has to reach the
			// storage too: that is what marks the edge as loaded.
			strong->storage().add(Storage::SharedMediaAddSlice(
				peer->id,
				MsgId(0),
				PeerId(0),
				type,
				std::move(parsed.messageIds),
				parsed.noSkipRange,
				parsed.fullCount));
		});
}

void Search(
		not_null<const void*> owner,
		Main::Session *session,
		PeerData *peer,
		Type type,
		const QString &query,
		MsgId messageId,
		Data::LoadDirection direction,
		Fn<void(Api::SearchResult)> done) {
	const auto kind = KindFor(type);
	const auto bridge = session ? BridgeFor(session) : nullptr;
	const auto history = (session && peer)
		? session->data().historyLoaded(peer)
		: nullptr;
	if (!bridge || !history || kind.isEmpty()) {
		// Deferred: the caller marks this gap as requested right after this
		// call, and a completion that ran before that would leave the mark
		// behind forever.
		crl::on_main([done = std::move(done)] { done({}); });
		return;
	}
	auto &state = SearchStates()[owner.get()];
	const auto requestId = ++state.lastId;
	state.live.emplace(requestId);
	const auto anchors = AnchorsFor(messageId, direction);
	const auto peerId = peer->id;
	const auto weak = base::make_weak(session);
	bridge->requestChatMedia(
		history,
		kind,
		query,
		anchors.before,
		anchors.after,
		anchors.around,
		kMediaLimit,
		[=, done = std::move(done)](NativeBridge::MediaPage page) {
			auto &states = SearchStates();
			const auto i = states.find(owner.get());
			if ((i == end(states)) || !i->second.live.remove(requestId)) {
				// The list was cancelled while this page was in flight.
				return;
			}
			const auto strong = weak.get();
			const auto peer = strong
				? strong->data().peerLoaded(peerId)
				: nullptr;
			if (!peer) {
				done({});
				return;
			}
			done(ResultFrom(peer, type, messageId, direction, page));
		});
}

mtpRequestId RequestGlobal(
		Main::Session *session,
		Type type,
		const QString &query,
		Data::MessagePosition offsetPosition,
		Fn<void(Api::GlobalMediaResult)> done) {
	const auto kind = KindFor(type);
	const auto bridge = session ? BridgeFor(session) : nullptr;
	if (!bridge || kind.isEmpty()) {
		// Deferred: the provider stores the returned id and compares the
		// completion against it, so finishing before it was stored would let
		// a stale answer through.
		crl::on_main([done] { done({}); });
		return kBridgeGlobalMarker;
	}
	// The provider pages by the position it last saw, and a global list is
	// ordered by message id, which is global here as well.
	const auto before = offsetPosition
		? qint64(offsetPosition.fullId.msg.bare)
		: qint64(0);
	bridge->requestGlobalMedia(
		kind,
		query,
		before,
		kMediaLimit,
		[=](NativeBridge::GlobalMediaPage page) {
			auto result = Api::GlobalMediaResult();
			result.fullCount = page.total;
			result.messageIds.reserve(page.ids.size());
			const auto owner = &session->data();
			for (const auto &fullId : page.ids) {
				const auto item = owner->message(fullId);
				if (!item) {
					continue;
				}
				result.offsetPosition = item->position();
				result.messageIds.push_back(item->position());
			}
			// offsetRate is the provider's "there is more" flag: it marks the
			// list loaded the moment this is zero. offsetPosition must stay
			// at the oldest item of the page even on the last one: the
			// provider treats an empty position as "nothing came back" and
			// throws the whole page away, which is what emptied a list that
			// fit in a single page.
			result.offsetRate = page.hasMore
				? int32(page.nextBefore)
				: 0;
			done(std::move(result));
		});
	return kBridgeGlobalMarker;
}

void CancelSearch(not_null<const void*> owner) {
	SearchStates().remove(owner.get());
}

} // namespace CustomBackend::SharedMedia
