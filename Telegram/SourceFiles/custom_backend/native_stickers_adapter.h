/*
This file is part of FoxMes Desktop.
*/
#pragma once

class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Api {
struct SendAction;
} // namespace Api

namespace CustomBackend::Stickers {

// Stickers. Upstream fills Data::Stickers from messages.getAllStickers, which
// never answers under the bridge, so the panel is built from the site's own
// catalog - the emojis table, served by GET /reactions and already loaded for
// the reaction strip.
//
// A sticker and a reaction are the same asset here: a 100x100 WebM on fxl-cdn.
// The difference is only in the document attributes. A reaction has to be a
// custom emoji (documentAttributeCustomEmoji) because that is what routes it
// through Ui::Text::CustomEmoji in the bubble; the panel wants a real sticker
// (documentAttributeSticker), which is what StickersListWidget lays out. The
// bytes behind both come from one download, shared through the reactions
// adapter.
//
// Sending one is not sending a file: on this product a sticker is a
// customEmoji node in the message document, exactly as fxl-web writes it, so
// the send is a text send carrying one custom_emoji entity.

// Fills Data::Stickers sets from the catalog. Answers the hook in
// ApiWrap::requestStickers.
void Request(not_null<Main::Session*> session);

// True when this adapter built the document.
[[nodiscard]] bool IsSticker(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document);

// Sends the sticker behind the document. Returns false when it is not ours,
// so the caller can fall through to upstream.
[[nodiscard]] bool Send(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

// Drops the documents built for a session that is going away.
void ClearSession(not_null<Main::Session*> session);

} // namespace CustomBackend::Stickers
