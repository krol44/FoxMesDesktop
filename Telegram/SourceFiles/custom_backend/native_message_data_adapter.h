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

// Serves Data::DownloadManager::resolve under the bridge. Upstream asks
// messages.getMessages for every downloaded file whose message is not in
// memory, which never answers here: resolveSentRequests stays positive, the
// persisted downloads are never restored, and the Downloads tab is hidden as
// empty after every restart. Calls finished() once, when every id of the
// group has been resolved.
void ResolveDownloadedMessages(
	Main::Session *session,
	const QVector<MTPInputMessage> &ids,
	Fn<void()> finished);

// Serves ApiWrap::requestSharedMedia under the bridge. Upstream answers it
// with messages.search over MTProto, which never completes here: the request
// would keep its slot in _sharedMediaRequests forever and leak an mtpRequestId
// that upstream cancels from a destructor that already ran. Pinned comes from
// the pinned list, the media lists come from custom_backend/
// native_shared_media_adapter, and a type FoxMes has no source for returns
// without a request, which leaves the viewer empty instead of hanging.
void RequestSharedMedia(
	Main::Session *session,
	PeerData *peer,
	Storage::SharedMediaType type,
	MsgId messageId,
	Data::LoadDirection direction);

} // namespace CustomBackend
