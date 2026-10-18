/*
This file is part of FoxMes Desktop.
*/
#pragma once

class DocumentData;
class QJsonObject;

namespace ChatHelpers {
class Show;
}

namespace Main {
class Session;
}

namespace Api {
struct SendAction;
}

namespace CustomBackend::Stickers {

void RequestFavorites(not_null<Main::Session*> session);
void ReceiveFavorites(
	not_null<Main::Session*> session,
	const QJsonObject &value);
void ResetFavorites(not_null<Main::Session*> session);
void ToggleFavorite(
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<DocumentData*> document,
	bool favorited);

[[nodiscard]] bool IsSticker(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document);

[[nodiscard]] bool Send(
	not_null<DocumentData*> document,
	const Api::SendAction &action);

void ClearSession(not_null<Main::Session*> session);

}
