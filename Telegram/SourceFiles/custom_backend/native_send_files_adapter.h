#pragma once

#include "api/api_common.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

namespace CustomBackend {

// Transport replacement for ApiWrap::sendFiles. Everything the send needs -
// reading the media kind out of the prepared files, building the upload specs,
// picking the caption - happens here, so the upstream hook stays a plain
// "if enabled, call this and return".
void SendFiles(
	Ui::PreparedList &&list,
	SendMediaType type,
	Api::SendAction action);

// Transport replacement for ApiWrap::sendFile - the clipboard path, which
// hands over raw bytes with no name. The real MIME is sniffed here so the
// attachment does not land as an unreadable application/octet-stream blob.
void SendFileContent(
	const QByteArray &content,
	SendMediaType type,
	const Api::SendAction &action);

// Transport replacement for ApiWrap::sendVoiceMessage - a recorded voice
// message, or a round video note when `video` is set. Neither goes through
// PreparedList at all: the recorder hands over finished bytes plus the
// metadata only it knows, so the upload specs are built here.
void SendVoiceMessage(
	const QByteArray &content,
	const VoiceWaveform &waveform,
	crl::time duration,
	bool video,
	const Api::SendAction &action);

// Transport replacement for Api::SendExistingDocument, which sends by
// InputDocument and has nothing to say under the bridge. A document is ours
// when this adapter built it - a saved GIF or a sticker - and only then is the
// send taken over; anything else falls through to upstream, which is what
// keeps the branch honest if a document from another source ever reaches here.
// Returns false when the document is not ours.
[[nodiscard]] bool SendExistingDocument(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

} // namespace CustomBackend
