/*
This file is part of FoxMes Desktop.
*/
#pragma once

class DocumentData;

namespace Main {
class Session;
}

namespace Api {
struct SendAction;
}

namespace CustomBackend::Stickers {


void Request(not_null<Main::Session*> session);

[[nodiscard]] bool IsSticker(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document);

[[nodiscard]] bool Send(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

void ClearSession(not_null<Main::Session*> session);

}
