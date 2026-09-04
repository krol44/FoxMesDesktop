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
struct CatalogItem final {
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
// v2 catalog: emoji list with per-entry CDN asset urls. Native Unicode alts
// are rendered from built-in graphics; unknown alts download their asset.
void SetAvailableCatalog(
	const QStringList &emojis,
	const QStringList &assetUrls,
	const QStringList &categories);

// The catalog as loaded, for surfaces other than the reaction strip.
[[nodiscard]] std::vector<CatalogItem> Catalog();
[[nodiscard]] rpl::producer<> CatalogChanged();

// The asset behind one alt, shared with the stickers panel so the same picture
// is downloaded once and rendered by whichever surface needs it.
[[nodiscard]] Asset AssetFor(const QString &emoji);
[[nodiscard]] rpl::producer<QString> AssetLoaded();

// The client-side DocumentId of one alt, minted and registered so the reverse
// lookup keeps working. It is derived from the alt alone and never leaves this
// client - see the "DocumentId is computed from the alt" rule in BRIDGE.md.
// A custom_emoji entity in a message resolves through it, which is what makes
// a sticker in a bubble the same object as the same sticker in the panel.
[[nodiscard]] DocumentId RegisterDocumentId(const QString &emoji);
[[nodiscard]] QString EmojiFor(DocumentId id);
// The CDN address of one alt, as the catalog serves it.
[[nodiscard]] QString AssetUrlFor(const QString &emoji);

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
