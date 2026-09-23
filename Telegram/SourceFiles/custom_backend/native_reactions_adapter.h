#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace Data {
struct Reaction;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace CustomBackend::Reactions {

// One catalog entry as the server serves it. It is the whole emoji table of
// the site, which is also what the stickers panel shows - reactions and
// stickers are one catalog here, not two.
//
// id is the identity of the row and the only key this client uses. emoji is
// its caption: it is unique inside a category and repeats between categories,
// so two different stickers legitimately carry the same characters.
struct CatalogItem final {
	DocumentId id = 0;
	QString emoji;
	QString assetUrl;
	QString category;
};

// The downloaded bytes of one entry, empty until its asset lands.
struct Asset final {
	QByteArray content;
	QString mime;
};

[[nodiscard]] MTPMessageReactions Build(
    Main::Session *session,
    const QJsonArray &reactions,
    qint64 myUserId);
// v2 catalog: catalog rows with per-entry CDN asset urls. The asset is the
// only source of the picture - see the "картинку реакции даёт только CDN"
// rule in BRIDGE.md.
//
// Everything below is per account, like upstream's Data::Reactions: the
// documents built from the catalog belong to one Data::Session and must never
// be handed to another.
void SetAvailableCatalog(
	not_null<Main::Session*> session,
	const std::vector<CatalogItem> &items);

// The caller's reaction order as the server keeps it (chat_user_emojis): most
// used, then most recently used, both as catalog ids. ApplyDefault turns them
// into the _top/_recent lists the picker is sorted by; upstream fills the
// same two lists from getTopReactions/getRecentReactions.
void SetUsageLists(
	not_null<Main::Session*> session,
	const std::vector<DocumentId> &top,
	const std::vector<DocumentId> &recent);

[[nodiscard]] std::vector<CatalogItem> Catalog(
	not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> CatalogChanged(
	not_null<Main::Session*> session);

// The asset behind one row, shared with the stickers panel so the same picture
// is downloaded once and rendered by whichever surface needs it.
[[nodiscard]] Asset AssetFor(
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

// Server-driven per-message limit of distinct chosen reactions
// (catalog "max_selected"). Defaults to the historical client value.
[[nodiscard]] int MaxSelectedReactions(not_null<Main::Session*> session);
void SetMaxSelectedReactions(not_null<Main::Session*> session, int value);
// Catalog -> native conversion: builds ready-to-use Data::Reaction values,
// so no conversion logic lives inside Data::Reactions. Every entry becomes a
// custom-emoji reaction, i.e. a ReactionId holding a DocumentId; entries
// whose asset is missing or still downloading are left out until it lands.
[[nodiscard]] std::vector<Data::Reaction> BuildAvailableReactions(
	not_null<Main::Session*> session);

// Drops everything kept for the account. Called when its session goes away,
// before Data::Session destroys the documents this state points to.
void ClearSession(not_null<Main::Session*> session);

} // namespace CustomBackend::Reactions
