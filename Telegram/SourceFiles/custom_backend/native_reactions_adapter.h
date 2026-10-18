#pragma once

#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_sticker_sets_adapter.h"
#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace Data {
struct Reaction;
}

namespace Main {
class Session;
}

namespace CustomBackend::Reactions {

using CatalogItem = StickerSets::Item;

[[nodiscard]] MTPMessageReactions Build(
    Main::Session *session,
    const QJsonArray &reactions,
    qint64 myUserId);
void SetAvailableCatalog(
	not_null<Main::Session*> session,
	const std::vector<CatalogItem> &items);

void SetUsageLists(
	not_null<Main::Session*> session,
	const std::vector<DocumentId> &top,
	const std::vector<DocumentId> &recent);

[[nodiscard]] std::vector<CatalogItem> Catalog(
	not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> CatalogChanged(
	not_null<Main::Session*> session);

[[nodiscard]] QString EmojiFor(
	not_null<Main::Session*> session,
	DocumentId id);
[[nodiscard]] QString AssetUrlFor(
	not_null<Main::Session*> session,
	DocumentId id);

[[nodiscard]] int MaxSelectedReactions(not_null<Main::Session*> session);
void SetMaxSelectedReactions(not_null<Main::Session*> session, int value);
[[nodiscard]] std::vector<Data::Reaction> BuildAvailableReactions(
	not_null<Main::Session*> session);

void RequestFavorite(not_null<Main::Session*> session);
void ReceiveFavorite(not_null<Main::Session*> session, const QJsonObject &state);
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId, const MTP::details::SerializedRequest &request);
void ClearSession(not_null<Main::Session*> session);

}
