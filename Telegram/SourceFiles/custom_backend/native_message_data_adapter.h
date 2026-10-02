#pragma once

#include "base/basic_types.h"
#include "data/data_messages.h"
#include "data/data_msg_id.h"
#include "storage/storage_shared_media.h"

#include <functional>

class PeerData;

namespace Main {
class Session;
}

namespace CustomBackend {

void RequestMessageData(
	Main::Session *session,
	PeerData *peer,
	MsgId messageId,
	std::function<void()> done);

void ResolveDownloadedMessages(
	Main::Session *session,
	const QVector<MTPInputMessage> &ids,
	Fn<void()> finished);

void RequestSharedMedia(
	Main::Session *session,
	PeerData *peer,
	Storage::SharedMediaType type,
	MsgId messageId,
	Data::LoadDirection direction);

}
