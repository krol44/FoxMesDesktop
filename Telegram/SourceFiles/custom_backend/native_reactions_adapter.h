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
void SetAvailableCatalog(const std::vector<CatalogItem> &items);

// The caller's reaction order as the server keeps it (chat_user_emojis): most
// used, then most recently used, both as catalog ids. ApplyDefault turns them
// into the _top/_recent lists the picker is sorted by; upstream fills the
// same two lists from getTopReactions/getRecentReactions.
void SetUsageLists(
	const std::vector<DocumentId> &top,
	const std::vector<DocumentId> &recent);

[[nodiscard]] std::vector<CatalogItem> Catalog();
[[nodiscard]] rpl::producer<> CatalogChanged();

// The asset behind one row, shared with the stickers panel so the same picture
// is downloaded once and rendered by whichever surface needs it.
[[nodiscard]] Asset AssetFor(DocumentId id);
[[nodiscard]] rpl::producer<DocumentId> AssetLoaded();

[[nodiscard]] QString EmojiFor(DocumentId id);
[[nodiscard]] QString AssetUrlFor(DocumentId id);

// Server-driven per-message limit of distinct chosen reactions
// (catalog "max_selected"). Defaults to the historical client value.
[[nodiscard]] int MaxSelectedReactions();
void SetMaxSelectedReactions(int value);
// Catalog -> native conversion: builds ready-to-use Data::Reaction values,
// so no conversion logic lives inside Data::Reactions. Every entry becomes a
// custom-emoji reaction, i.e. a ReactionId holding a DocumentId; entries
// whose asset is missing or still downloading are left out until it lands.
[[nodiscard]] std::vector<Data::Reaction> BuildAvailableReactions(
	Main::Session *session);

} // namespace CustomBackend::Reactions
