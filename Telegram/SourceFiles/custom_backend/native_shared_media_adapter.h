/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_messages.h"
#include "storage/storage_shared_media.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Api {
struct SearchResult;
struct GlobalMediaResult;
} // namespace Api

namespace CustomBackend::SharedMedia {

// True for the media types FoxMes can serve. Everything else must return
// without touching the network: an unsupported request that still went out
// would leave upstream waiting for a page that never arrives.
[[nodiscard]] bool Supported(Storage::SharedMediaType type);

// Serves ApiWrap::requestSharedMedia for everything but Pinned: fetches one
// page around the requested position and feeds it into Storage::SharedMedia,
// which is where the tab list and the count under its title both read from.
void Request(
	Main::Session *session,
	PeerData *peer,
	Storage::SharedMediaType type,
	MsgId messageId,
	Data::LoadDirection direction);

// Serves Api::SearchController::requestMore, the in-tab search over Files,
// Links and Music. The result is handed back instead of stored, because that
// list lives in the controller rather than in Storage::SharedMedia.
void Search(
	not_null<const void*> owner,
	Main::Session *session,
	PeerData *peer,
	Storage::SharedMediaType type,
	const QString &query,
	MsgId messageId,
	Data::LoadDirection direction,
	Fn<void(Api::SearchResult)> done);
void CancelSearch(not_null<const void*> owner);

// Serves ApiWrap::requestGlobalMedia - the media tabs of the chat-list
// search, which span every chat of the account. The returned marker is not a
// real mtpRequestId: Info::GlobalMedia::Provider only compares it against the
// one it stored and cancels it, and cancelling an unknown id is a no-op.
[[nodiscard]] mtpRequestId RequestGlobal(
	Main::Session *session,
	Storage::SharedMediaType type,
	const QString &query,
	Data::MessagePosition offsetPosition,
	Fn<void(Api::GlobalMediaResult)> done);

} // namespace CustomBackend::SharedMedia
