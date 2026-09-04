#include "custom_backend/native_message_data_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_shared_media_adapter.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"

#include <memory>
#include <utility>
#include <vector>

namespace CustomBackend {

void RequestMessageData(
		Main::Session *session,
		PeerData *peer,
		MsgId messageId,
		std::function<void()> done) {
	if (const auto bridge = BridgeFor(session)) {
		bridge->requestMessageData(
			peer,
			messageId,
			std::move(done));
	} else if (done) {
		done();
	}
}

void ResolveDownloadedMessages(
		Main::Session *session,
		const QVector<MTPInputMessage> &ids,
		Fn<void()> finished) {
	const auto bridge = BridgeFor(session);
	auto pending = std::vector<MsgId>();
	if (bridge) {
		for (const auto &id : ids) {
			if (id.type() == mtpc_inputMessageID) {
				pending.push_back(MsgId(id.c_inputMessageID().vid().v));
			}
		}
	}
	if (pending.empty()) {
		if (finished) {
			finished();
		}
		return;
	}
	// The peer is left out on purpose: the response carries chat_id, and the
	// bridge routes the message by it. The grouping upstream builds is a
	// MTProto batching detail, not the real owner of these messages.
	const auto left = std::make_shared<int>(int(pending.size()));
	for (const auto messageId : pending) {
		bridge->requestMessageData(nullptr, messageId, [=] {
			if (!--*left && finished) {
				finished();
			}
		});
	}
}

void RequestSharedMedia(
		Main::Session *session,
		PeerData *peer,
		Storage::SharedMediaType type,
		MsgId messageId,
		Data::LoadDirection direction) {
	if (!peer) {
		return;
	} else if (type == Storage::SharedMediaType::Pinned) {
		if (const auto bridge = BridgeFor(session)) {
			bridge->requestPinnedMessages(session->data().historyLoaded(peer));
		}
		return;
	}
	SharedMedia::Request(session, peer, type, messageId, direction);
}

} // namespace CustomBackend
