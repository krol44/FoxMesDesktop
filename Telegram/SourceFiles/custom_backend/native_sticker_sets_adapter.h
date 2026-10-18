/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "custom_backend/native_mtp_router.h"

#include <QtCore/QJsonObject>

class DocumentData;

namespace Main {
class Session;
}

namespace CustomBackend::StickerSets {

struct Item final {
	DocumentId id = 0;
	QString emoji;
	QString url;
	QString mime;
	int width = 0;
	int height = 0;
	int64 size = 0;
	uint64 setId = 0;
	bool sticker = false;
	bool textColor = false;
};

[[nodiscard]] std::optional<Item> ParseItem(
	const QJsonObject &value,
	bool sticker);
[[nodiscard]] DocumentId DocumentIdFor(const Item &item);
[[nodiscard]] DocumentId CatalogId(DocumentId documentId);
[[nodiscard]] bool IsCatalogSticker(DocumentId documentId);

[[nodiscard]] MTPDocument Document(
	not_null<Main::Session*> session,
	const Item &item);
not_null<DocumentData*> Register(
	not_null<Main::Session*> session,
	const Item &item);
[[nodiscard]] std::optional<Item> Find(
	not_null<Main::Session*> session,
	DocumentId documentId);
[[nodiscard]] std::optional<MTPMessageMedia> MessageMedia(
	not_null<Main::Session*> session,
	const QJsonObject &sticker);

void Load(not_null<Main::Session*> session);
void Reload(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> Updated(not_null<Main::Session*> session);
void ApplyEvent(not_null<Main::Session*> session, const QJsonObject &state);

[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

void ClearSession(not_null<Main::Session*> session);

}
