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
} // namespace Main

namespace CustomBackend::Search {

// REST peer search: parses JSON and builds Api::PeerSearchResult here,
// so no JSON handling or DTO conversion lives inside Api::PeerSearch.
void SearchPeers(
	not_null<Main::Session*> session,
	const QString &query,
	Fn<void(Api::PeerSearchResult)> done);

// Messages search transport: owns pagination state keyed by the owner
// pointer. Upstream passes the same request token for every page, so the
// cursor (server next_before) is tracked here: a new query restarts it, a
// repeated call continues from it, and has_more=false ends the pagination
// without further network requests. Stale responses are dropped via a per
// owner generation counter.
void RequestMessages(
	not_null<const void*> owner,
	not_null<History*> history,
	const QString &query,
	const QString &nextToken,
	Fn<void(Api::FoundMessages)> done);
[[nodiscard]] bool MessagesPending(not_null<const void*> owner);
void CancelMessages(not_null<const void*> owner);

// Peer search shaped as the upstream response. ShareBox and the peer-list
// controllers hand contacts.Found straight to their own handlers, so the
// bridge answers in that shape and their handling stays untouched. The users
// and chats vectors are empty on purpose: the peers are already built here,
// and both callers read them through peerLoaded().
//
// The returned value is not a real mtpRequestId: it stands in for one, and it
// is unique per call so the caller can tell its own answer from a superseded
// one exactly the way it does with a real request id. The same marker is
// handed back to the callback.
[[nodiscard]] mtpRequestId SearchPeersFound(
	not_null<Main::Session*> session,
	const QString &query,
	Fn<void(MTPcontacts_Found, mtpRequestId)> done);

// Chat-list search over FoxMes messages, shaped as the upstream response so
// Dialogs::Widget::searchReceived keeps its paging, caching and full-flag
// handling. inPeer is null for a search across every chat. The marker works
// like the one above.
[[nodiscard]] mtpRequestId SearchInChats(
	not_null<Main::Session*> session,
	const QString &query,
	PeerData *inPeer,
	qint64 before,
	Fn<void(MTPmessages_Messages, mtpRequestId)> done);

} // namespace CustomBackend::Search
