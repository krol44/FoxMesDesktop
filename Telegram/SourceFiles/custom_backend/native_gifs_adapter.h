/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QString>

class DocumentData;
class History;

namespace Main {
class Session;
} // namespace Main

namespace Api {
struct SendAction;
} // namespace Api

namespace Data {
struct FileOrigin;
} // namespace Data

namespace CustomBackend::Gifs {

// Saved GIFs. Upstream fills Data::Stickers::savedGifs() from
// messages.getSavedGifs and toggles one with messages.saveGif; neither answers
// under the bridge, so the list comes from GET /gifs and the toggle from
// POST /gifs and DELETE /gifs/{id}.
//
// What the panel shows is an mp4 in every case. The chat never stores a gif as
// a gif: the upload renders one to mp4 and keeps calling the result a gif, so
// the document carries the animated marker and upstream loops it exactly as it
// loops a Telegram GIF.

// Fills Data::Stickers::savedGifs() from the server. Answers the hook in
// ApiWrap::requestSavedGifs.
void Request(not_null<Main::Session*> session);

// Records where the bytes of a document live, so a "save GIF" on a message can
// name the file to the server. DocumentData keeps the content url privately
// and the sha256 not at all, and the bridge is the only place that ever knew
// both - so it remembers them here as it builds each attachment.
void RememberSource(not_null<Main::Session*> session, DocumentId documentId, const QString &sha256);

// The sha256 behind a document, empty when it is not one of ours.
[[nodiscard]] QString SourceSha256(not_null<Main::Session*> session, DocumentId documentId);

// Adds or removes one saved GIF. The origin says where the picture was seen:
// the server copies somebody else's file to the caller from that chat, which
// is what makes "save GIF" work on a message you did not send.
void Toggle(
	not_null<DocumentData*> document,
	Data::FileOrigin origin,
	bool saved);

// Sends a saved GIF into a chat. Answers the hook in
// HistoryWidget::sendExistingDocument for a document of ours.
[[nodiscard]] bool Send(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

// True when the document was built by this adapter, i.e. it is a saved GIF and
// not an arbitrary animation from the history.
[[nodiscard]] bool IsSavedGif(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document);

// Drops everything this adapter remembers about a session. The DocumentData it
// points at belongs to the Data::Session going away, so the next login has to
// rebuild every entry against its own objects instead of reusing dangling
// pointers.
void ClearSession(not_null<Main::Session*> session);

} // namespace CustomBackend::Gifs
