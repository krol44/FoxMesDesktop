#pragma once

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

struct CatalogItem final {
	DocumentId id = 0;
	QString emoji;
	QString assetUrl;
	QString category;
};

struct Asset final {
	QByteArray content;
	QString mime;
};

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

[[nodiscard]] Asset AssetFor(
	not_null<Main::Session*> session,
	DocumentId id);
[[nodiscard]] bool IsSourceWebm(
	not_null<Main::Session*> session,
	DocumentId id);
[[nodiscard]] rpl::producer<DocumentId> AssetLoaded(
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

void ClearSession(not_null<Main::Session*> session);

}
