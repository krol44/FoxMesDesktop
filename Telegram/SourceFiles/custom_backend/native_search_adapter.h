/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_messages_search.h"
#include "api/api_peer_search.h"

#include <QString>

class History;
class PeerData;

namespace Main {
class Session;
}

namespace CustomBackend::Search {

void SearchPeers(
	not_null<Main::Session*> session,
	const QString &query,
	Fn<void(Api::PeerSearchResult)> done);

void RequestMessages(
	not_null<const void*> owner,
	not_null<History*> history,
	const QString &query,
	const QString &nextToken,
	Fn<void(Api::FoundMessages)> done);
[[nodiscard]] bool MessagesPending(not_null<const void*> owner);
void CancelMessages(not_null<const void*> owner);

[[nodiscard]] mtpRequestId SearchPeersFound(
	not_null<Main::Session*> session,
	const QString &query,
	Fn<void(MTPcontacts_Found, mtpRequestId)> done);

[[nodiscard]] mtpRequestId SearchInChats(
	not_null<Main::Session*> session,
	const QString &query,
	PeerData *inPeer,
	qint64 before,
	Fn<void(MTPmessages_Messages, mtpRequestId)> done);

}
