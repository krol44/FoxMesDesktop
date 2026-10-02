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

[[nodiscard]] bool SendExistingDocument(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

}
