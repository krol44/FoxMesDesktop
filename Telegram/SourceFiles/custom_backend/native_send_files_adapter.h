#pragma once

#include "api/api_common.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

namespace CustomBackend {

void SendFiles(
	Ui::PreparedList &&list,
	SendMediaType type,
	Api::SendAction action);

void SendFileContent(
	const QByteArray &content,
	SendMediaType type,
	const Api::SendAction &action);

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
