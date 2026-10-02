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
}

namespace Api {
struct SearchResult;
struct GlobalMediaResult;
}

namespace CustomBackend::SharedMedia {

[[nodiscard]] bool Supported(Storage::SharedMediaType type);

void Request(
	Main::Session *session,
	PeerData *peer,
	Storage::SharedMediaType type,
	MsgId messageId,
	Data::LoadDirection direction);

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

[[nodiscard]] mtpRequestId RequestGlobal(
	Main::Session *session,
	Storage::SharedMediaType type,
	const QString &query,
	Data::MessagePosition offsetPosition,
	Fn<void(Api::GlobalMediaResult)> done);

}
