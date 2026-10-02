/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QString>

class DocumentData;
class History;

namespace Main {
class Session;
}

namespace Api {
struct SendAction;
}

namespace Data {
struct FileOrigin;
}

namespace CustomBackend::Gifs {


void Request(not_null<Main::Session*> session);

void RememberSource(not_null<Main::Session*> session, DocumentId documentId, const QString &sha256);

[[nodiscard]] QString SourceSha256(not_null<Main::Session*> session, DocumentId documentId);

void Toggle(
	not_null<DocumentData*> document,
	Data::FileOrigin origin,
	bool saved);

[[nodiscard]] bool Send(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

[[nodiscard]] bool IsSavedGif(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document);

void ClearSession(not_null<Main::Session*> session);

}
