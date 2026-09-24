#include "custom_backend/native_reactions_adapter.h"

#include "base/flat_map.h"
#include "base/unixtime.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "history/history_item.h"
#include "data/data_message_reactions.h"
#include "data/data_message_reaction_id.h"
#include "data/data_peer_id.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "storage/file_download.h"
#include "ui/emoji_config.h"
#include "ui/text/text_utilities.h"
#include "main/main_session.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QUrl>

namespace CustomBackend::Reactions {
namespace {

constexpr auto kStaticEmojiSize = 100;

// Two requests per catalog entry: the source, which is the fallback when the
// alt carries no animation, and the emoji_webm rendition, which is preferred
// whenever fxl-cdn really had something to transcode.
//
// The document is built once, after both answers are in, and never rebuilt
// from the other variant: a reaction is one custom-emoji DocumentId, and
// CustomEmojiManager caches a Ui::CustomEmoji::Instance per id, so swapping
// the content under a live id would leave already-painted badges on the old
// renderer until restart. The only case that still swaps is a retry after a
// failed emoji_webm request, which is why that failure is retryable.
struct PendingAsset final {
    QByteArray source; // Normalized still, empty when unusable.
    QByteArray animated; // WebM, empty when the alt has no animation.
    bool sourceDone = false;
    bool animatedDone = false;
};

// Everything the bridge knows about reactions, per account - the same split
// upstream has, where Data::Reactions and CustomEmojiManager belong to one
// Data::Session. Nothing here may be shared between two logged in accounts:
// a DocumentData is an object of one Data::Session, and a document handed to
// the other account is never announced to its CustomEmojiManager, so every
// reaction and sticker message there stays an empty placeholder.
struct State final {
    std::vector<CatalogItem> catalog;
    int maxSelected = 3;

    // The two usage orders the server keeps per user (chat_user_emojis):
    // most used and most recently used, as catalog ids. Upstream fills the
    // same two lists from messages.getTopReactions/getRecentReactions, and
    // the picker reads them as concat(top, recent, full) - see
    // LookupPossibleReactions.
    std::vector<DocumentId> topUsage;
    std::vector<DocumentId> recentUsage;

    // Bridge reactions are custom-emoji reactions: upstream identifies them
    // by DocumentId, not by the emoji string, and that is what routes the
    // inline badge through Ui::Text::CustomEmoji instead of the single cached
    // frame Reactions::resolveImageFor() bakes for plain emoji reactions.
    //
    // The DocumentId is the emojis.id the server sent: it arrives inside the
    // payload itself, so Build() can answer synchronously, long before (or
    // without ever) the asset behind it downloading. Minting it from the
    // emoji is not an option - the same characters legitimately sit in two
    // catalog groups and would collapse into one document.
    //
    // The emoji of a row, for the text a send carries under its entity.
    // Filled from the catalog, so a row that ever reached upstream resolves
    // without touching Data::Session.
    base::flat_map<DocumentId, QString> emojiByDocumentId;

    // The media view that owns each reaction document's bytes. A reaction
    // document exists only in memory: nothing uploads or downloads it, so the
    // only copy of its content is the one held here.
    base::flat_map<DocumentId, std::shared_ptr<Data::DocumentMedia>> media;
    base::flat_map<DocumentId, not_null<DocumentData*>> documents;

    // The asset bytes behind each row. Kept so the stickers panel can build
    // its own documents from them: it needs the same picture carrying sticker
    // attributes instead of custom-emoji ones, and downloading the whole
    // catalog a second time for that would double the traffic of every login.
    base::flat_map<DocumentId, Asset> assets;
    base::flat_map<DocumentId, PendingAsset> pending;
    QSet<DocumentId> sourceWebmIds;
    QSet<QString> requested;

    // Fires once per row whose asset just landed, so a panel built before the
    // download finished can rebuild instead of staying empty until the next
    // catalog refresh.
    rpl::event_stream<DocumentId> assetLoaded;
    rpl::event_stream<> catalogChanged;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<State>> &States() {
    static auto value = base::flat_map<
        not_null<Main::Session*>,
        std::unique_ptr<State>>();
    return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
    auto &slot = States()[session];
    if (!slot) {
        slot = std::make_unique<State>();
    }
    return *slot;
}

// For network answers: the account may have logged out while the request was
// in flight, and recreating its state then would leave an entry keyed by a
// dead session that the next one allocated at the same address inherits.
[[nodiscard]] State *FindState(Main::Session *session) {
    const auto i = session ? States().find(session) : States().end();
    return (i == States().end()) ? nullptr : i->second.get();
}

// Renders a native Unicode emoji into a transparent square WebP that the ink
// fills edge to edge, which is how the custom-emoji renderer wants it: the
// inline badge paints a custom frame at AdjustCustomEmojiSize(st::emojiSize)
// and st::emojiSize equals st::reactionInlineSize, so no padding is needed.
[[nodiscard]] QByteArray RasterizeNativeEmoji(const QString &emoji) {
    const auto found = Ui::Emoji::Find(emoji);
    if (!found) {
        return {};
    }
    // Ui::Emoji::Draw() only accepts the two prepared instance sizes and
    // asserts on anything else, so the fixed-size static image is rendered
    // from the universal sprites, which scale to an arbitrary size.
    const auto images = Ui::Emoji::SourceImages();
    if (!images || !images->ensureLoaded()) {
        // Without the sprites nothing is cached and the next selector
        // rebuild retries the rasterization.
        return {};
    }
    auto image = QImage(
        kStaticEmojiSize,
        kStaticEmojiSize,
        QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    auto p = QPainter(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    images->draw(p, found, kStaticEmojiSize, 0, 0);
    p.end();
    auto buffer = QByteArray();
    auto sink = QBuffer(&buffer);
    sink.open(QIODevice::WriteOnly);
    if (!image.save(&sink, "WEBP", 90)) {
        return {};
    }
    return buffer;
}

[[nodiscard]] QByteArray NormalizeToSquareWebp(QByteArray bytes) {
    auto image = QImage::fromData(bytes, "WEBP");
    if (image.isNull()) {
        image = QImage::fromData(bytes);
    }
    if (image.isNull()) {
        return {};
    }
    // Transparent square canvas: a non-square asset must not stretch.
    const auto side = std::max(image.width(), image.height());
    auto square = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
    square.fill(Qt::transparent);
    auto p = QPainter(&square);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(
        QRect(
            (side - image.width()) / 2,
            (side - image.height()) / 2,
            image.width(),
            image.height()),
        image);
    p.end();
    auto buffer = QByteArray();
    auto sink = QBuffer(&buffer);
    sink.open(QIODevice::WriteOnly);
    if (!square.save(&sink, "WEBP", 90)) {
        return {};
    }
    return buffer;
}

// Builds the in-memory custom-emoji document behind one reaction.
//
// documentAttributeCustomEmoji, not documentAttributeSticker: it is what
// makes DocumentData::setattributes() write StickersType::Emoji, and the
// free flag is mandatory - without it the same call raises PremiumSticker
// and the whole catalog disappears behind the premium gate.
//
// The sticker alt carries the emoji: upstream maps a custom reaction back to
// its plain counterpart through sticker()->alt
// (Reactions::chooseGenericAnimation, Reactions::preloadImageFor). The row it
// belongs to is the id, which is what every cache here is keyed by.
[[nodiscard]] DocumentData *CustomEmojiDocument(
        not_null<Main::Session*> session,
        State &state,
        DocumentId id,
        const QString &emoji,
        const QByteArray &content,
        const QString &mime) {
    if (content.isEmpty()) {
        return nullptr;
    }
    using Flag = MTPDdocumentAttributeCustomEmoji::Flag;
    const auto attributes = QVector<MTPDocumentAttribute>{
        MTP_documentAttributeFilename(MTP_string(u"reaction"_q)),
        // Declared, not measured: reaction assets are square, and the real
        // frame size comes from the decoder at paint time anyway.
        MTP_documentAttributeImageSize(
            MTP_int(kStaticEmojiSize),
            MTP_int(kStaticEmojiSize)),
        MTP_documentAttributeCustomEmoji(
            MTP_flags(Flag::f_free),
            MTP_string(emoji),
            MTP_inputStickerSetEmpty()),
    };
    const auto document = session->data().document(
        id,
        uint64(0), // access hash
        QByteArray(), // file reference
        base::unixtime::now(),
        attributes,
        mime,
        InlineImageLocation(),
        ImageWithLocation(),
        ImageWithLocation(),
        false, // isPremiumSticker
        0, // dc
        int64(content.size()));
    if (!document->sticker()) {
        return nullptr;
    }
    // Nothing ever uploads or downloads this document, so DocumentMedia has
    // no way to obtain its content: without the bytes handed to a media view
    // that stays alive, loaded() is false, CustomEmojiLoader never reaches
    // its frame generator and the reaction renders as an empty placeholder.
    auto media = document->createMediaView();
    media->setBytes(content);
    state.media[id] = std::move(media);
    state.documents.insert_or_assign(id, document);
    // A reaction can be painted before its asset exists: the badge asks
    // CustomEmojiManager for this id, finds no document and parks a loader
    // in the resolve state, which under the bridge nothing would ever
    // complete. Announcing the document here is what finishes those.
    session->data().customEmojiManager().resolveLocalDocument(document);
    return document;
}

// fxl-cdn transcodes an animated source to WebM/VP9 under this op; a source
// without animation comes back untouched. See the CDN README, section
// "emoji_webm".
[[nodiscard]] QString AnimatedUrl(const QString &assetUrl) {
    return assetUrl + u"/-/emoji_webm/"_q;
}


QNetworkAccessManager &AssetManager() {
    static auto manager = std::make_unique<QNetworkAccessManager>();
    [[maybe_unused]] static const auto release = [] {
        ReleaseOnQuit([] { manager = nullptr; });
        return true;
    }();

    Ensures(manager != nullptr);
    return *manager;
}

// Fetches one CDN asset without any bearer token: reaction assets are public
// catalog data, the token must never leak to the CDN. `apply` runs on the
// main thread for every outcome, a transport error included, so a caller
// waiting on two responses always gets both; returning false means the asset
// was unusable and the request may be retried on the next catalog refresh.
// Nothing runs once the account has logged out in the meantime.
//
// Deduplicated per account, not per process: upstream has every session ask
// for its own reactions, and an answer can only build documents for the
// session that asked. The transport is the one shared part, like upstream's
// WebLoadManager.
void FetchAsset(
        base::weak_ptr<Main::Session> weakSession,
        const QString &url,
        Fn<bool(
            not_null<Main::Session*> session,
            State &state,
            bool ok,
            const QByteArray &body,
            const QString &contentType)> apply) {
    const auto session = weakSession.get();
    const auto state = FindState(session);
    if (!state || state->requested.contains(url)) {
        return;
    }
    state->requested.insert(url);
    // The dev profile needs the documented TLS exception, because fxl-cdn
    // serves a certificate that does not match cdn.fxl.test and every asset
    // would fail the handshake.
    const auto auth = DownloadAuth{ .insecureTls = DevInsecureTls() };
    auto request = QNetworkRequest(QUrl(url));
    ApplyDownloadAuth(request, auth);
    const auto reply = AssetManager().get(request);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply, auth] {
        AllowDownloadTls(reply, auth);
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
        reply->deleteLater();
        const auto session = weakSession.get();
        const auto state = FindState(session);
        if (!state) {
            return;
        }
        const auto ok = (reply->error() == QNetworkReply::NoError);
        const auto contentType = ok
            ? reply->header(QNetworkRequest::ContentTypeHeader).toString()
            : QString();
        const auto body = ok ? reply->readAll() : QByteArray();
        if (!apply(session, *state, ok, body, contentType)) {
            state->requested.remove(url);
        }
    });
}

void RefreshReactions(not_null<Main::Session*> session) {
    crl::on_main(session.get(), [=] {
        session->data().reactions().refreshDefault();
    });
}

void AssetBuilt(
        not_null<Main::Session*> session,
        State &state,
        DocumentId id,
        Asset asset) {
    state.assets[id] = std::move(asset);
    RefreshReactions(session);
    state.assetLoaded.fire_copy(id);
}

void BuildDownloadedAsset(
        not_null<Main::Session*> session,
        State &state,
        DocumentId id,
        const QString &emoji) {
    const auto i = state.pending.find(id);
    if (i == state.pending.end()
        || !i->second.sourceDone
        || !i->second.animatedDone) {
        return;
    }
    const auto pending = i->second;
    state.pending.erase(i);
    const auto animated = !pending.animated.isEmpty();
    const auto content = animated ? pending.animated : pending.source;
    const auto mime = animated ? u"video/webm"_q : u"image/webp"_q;
    if (CustomEmojiDocument(session, state, id, emoji, content, mime)) {
        AssetBuilt(session, state, id, { .content = content, .mime = mime });
    }
}

void DownloadAsset(
        not_null<Main::Session*> session,
        State &state,
        DocumentId id,
        const QString &emoji,
        const QString &url) {
    if (!state.pending.contains(id)) {
        state.pending.emplace(id, PendingAsset());
    }
    const auto weak = base::make_weak(session);

    FetchAsset(weak, url, [id, emoji](
            not_null<Main::Session*> session,
            State &state,
            bool ok,
            const QByteArray &body,
            const QString &contentType) {
        // A video sticker is stored on fxl-cdn as WebM already: the source is
        // the animation itself and has no still to normalize.
        const auto video = ok && contentType.startsWith(u"video/webm"_q);
        const auto normalized = (ok && !video)
            ? NormalizeToSquareWebp(body)
            : QByteArray();
        const auto i = state.pending.find(id);
        if (i == state.pending.end()) {
            return ok;
        }
        if (video) {
            state.sourceWebmIds.insert(id);
        } else {
            state.sourceWebmIds.remove(id);
        }
        i->second.source = normalized;
        if (video) {
            i->second.animated = body;
        }
        i->second.sourceDone = true;
        BuildDownloadedAsset(session, state, id, emoji);
        return ok && (video || !normalized.isEmpty());
    });

    FetchAsset(weak, AnimatedUrl(url), [id, emoji](
            not_null<Main::Session*> session,
            State &state,
            bool ok,
            const QByteArray &body,
            const QString &contentType) {
        // A source without animation comes back echoed as the original, so
        // only the transcoded mime marks a usable rendition.
        const auto webm = (ok && contentType.startsWith(u"video/webm"_q))
            ? body
            : QByteArray();
        const auto i = state.pending.find(id);
        if (i != state.pending.end()) {
            // Never clears: a WebM source may have filled it already.
            if (!webm.isEmpty()) {
                i->second.animated = webm;
            }
            i->second.animatedDone = true;
            BuildDownloadedAsset(session, state, id, emoji);
        } else if (!webm.isEmpty()) {
            // A retry that finally produced the animation: rebuild over the
            // still that was put in place while this request kept failing.
            const auto mime = u"video/webm"_q;
            if (CustomEmojiDocument(session, state, id, emoji, webm, mime)) {
                AssetBuilt(
                    session,
                    state,
                    id,
                    { .content = webm, .mime = mime });
            }
        }
        // A transport error is worth another attempt; an echoed original is
        // the CDN's final answer for this row.
        return ok;
    });
}

struct Aggregate final {
    int count = 0;
    bool chosen = false;
    QVector<qint64> recentUserIds;
};

QVector<MTPReactionCount> BuildCounts(
        const QHash<DocumentId, Aggregate> &aggregates,
        const QVector<DocumentId> &order) {
    auto result = QVector<MTPReactionCount>();
    auto chosenOrder = 0;
    for (const auto emojiId : order) {
        const auto i = aggregates.find(emojiId);
        if (i == aggregates.end() || !emojiId || i->count <= 0) {
            continue;
        }
        using Flag = MTPDreactionCount::Flag;
        const auto flags = i->chosen ? Flag::f_chosen_order : Flag();
        result.push_back(MTP_reactionCount(
            MTP_flags(flags),
            i->chosen ? MTP_int(++chosenOrder) : MTPint(),
            Data::ReactionToMTP(Data::ReactionId{ emojiId }),
            MTP_int(i->count)));
    }
    return result;
}

QVector<MTPMessagePeerReaction> BuildRecent(
        const QHash<DocumentId, Aggregate> &aggregates,
        const QVector<DocumentId> &order,
        qint64 myUserId) {
    auto result = QVector<MTPMessagePeerReaction>();
    const auto now = base::unixtime::now();
    for (const auto emojiId : order) {
        const auto i = aggregates.find(emojiId);
        if (i == aggregates.end() || !emojiId) {
            continue;
        }
        auto seen = QSet<qint64>();
        for (const auto userId : i->recentUserIds) {
            if (userId <= 0 || seen.contains(userId)) {
                continue;
            }
            seen.insert(userId);
            using Flag = MTPDmessagePeerReaction::Flag;
            using Flags = base::flags<Flag>;
            auto flags = Flags();
            if (userId == myUserId) {
                flags |= Flag::f_my;
            }
            result.push_back(MTP_messagePeerReaction(
                MTP_flags(flags),
                peerToMTP(peerFromUser(UserId(userId))),
                MTP_int(now),
                Data::ReactionToMTP(Data::ReactionId{ emojiId })));
        }
    }
    return result;
}

QPair<QHash<DocumentId, Aggregate>, QVector<DocumentId>> Normalize(
        const QJsonArray &reactions,
        qint64 myUserId) {
    auto aggregates = QHash<DocumentId, Aggregate>();
    auto order = QVector<DocumentId>();
    for (const auto &entry : reactions) {
        const auto object = entry.toObject();
        const auto emojiId = DocumentId(
            object.value("emoji_id").toVariant().toLongLong());
        if (!emojiId) {
            continue;
        }
        if (!aggregates.contains(emojiId)) {
            order.push_back(emojiId);
        }
        auto &aggregate = aggregates[emojiId];
        const auto explicitCount = object.value("count").toInt();
        aggregate.count += (explicitCount > 0) ? explicitCount : 1;
        aggregate.chosen = aggregate.chosen
            || object.value("chosen").toBool()
            || (object.value("user_id").toVariant().toLongLong() == myUserId);
        const auto recent = object.value("recent_user_ids").toArray();
        for (const auto &user : recent) {
            const auto userId = user.toVariant().toLongLong();
            if (userId > 0) {
                aggregate.recentUserIds.push_back(userId);
            }
        }
        const auto legacyUserId = object.value("user_id").toVariant().toLongLong();
        if (legacyUserId > 0) {
            aggregate.recentUserIds.push_back(legacyUserId);
        }
    }
    return { aggregates, order };
}

} // namespace

MTPMessageReactions Build(
        Main::Session* /*session*/,
        const QJsonArray &reactions,
        qint64 myUserId) {
    const auto normalized = Normalize(reactions, myUserId);
    const auto counts = BuildCounts(normalized.first, normalized.second);
    const auto recent = BuildRecent(normalized.first, normalized.second, myUserId);
    using Flag = MTPDmessageReactions::Flag;
    using Flags = base::flags<Flag>;
    // A channel post's reactions come as counts without a user_id: nobody
    // may see who reacted, as with upstream's broadcast channels.
    const auto anonymous = ranges::any_of(reactions, [](const QJsonValue &v) {
        const auto object = v.toObject();
        return (object.value("count").toInt() > 0)
            && !object.value("user_id").toVariant().toLongLong();
    });
    auto flags = anonymous ? Flags() : Flags(Flag::f_can_see_list);
    if (!recent.isEmpty()) {
        flags |= Flag::f_recent_reactions;
    }
    return MTP_messageReactions(
        MTP_flags(flags),
        MTP_vector<MTPReactionCount>(counts),
        recent.isEmpty()
            ? MTPVector<MTPMessagePeerReaction>()
            : MTP_vector<MTPMessagePeerReaction>(recent),
        MTPVector<MTPMessageReactor>());
}

void SetAvailableCatalog(
        not_null<Main::Session*> session,
        const std::vector<CatalogItem> &items) {
    auto entries = std::vector<CatalogItem>();
    entries.reserve(items.size());
    for (const auto &item : items) {
        if (!item.id || item.emoji.isEmpty()) {
            continue;
        }
        entries.push_back(item);
    }
    if (entries.empty()) {
        return;
    }
    // The emoji of every row is registered here, not when a document is
    // built: SendChosen() and the outgoing entities need it for rows whose
    // asset is still downloading.
    auto &state = StateFor(session);
    for (const auto &entry : entries) {
        state.emojiByDocumentId[entry.id] = entry.emoji;
    }
    state.catalog = std::move(entries);
    state.catalogChanged.fire({});
}

void SetUsageLists(
        not_null<Main::Session*> session,
        const std::vector<DocumentId> &top,
        const std::vector<DocumentId> &recent) {
    auto &state = StateFor(session);
    state.topUsage = top;
    state.recentUsage = recent;
}

int MaxSelectedReactions(not_null<Main::Session*> session) {
    return StateFor(session).maxSelected;
}

void SetMaxSelectedReactions(not_null<Main::Session*> session, int value) {
    if (value < 1) {
        return;
    }
    StateFor(session).maxSelected = value;
}

std::vector<Data::Reaction> BuildAvailableReactions(
		not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	auto result = std::vector<Data::Reaction>();
	result.reserve(state.catalog.size());
	for (const auto &entry : state.catalog) {
		if (entry.emoji.isEmpty()) {
			continue;
		}
		const auto i = state.documents.find(entry.id);
		auto icon = (i == state.documents.end())
			? nullptr
			: i->second.get();
		if (!icon) {
			if (!entry.assetUrl.isEmpty()) {
				// The uploaded artwork is what this row is. The emoji is only
				// its caption, so recognising it as a native Unicode emoji
				// must not replace the picture with a font glyph.
				DownloadAsset(
					session,
					state,
					entry.id,
					entry.emoji,
					entry.assetUrl);
			} else if (Ui::Emoji::Find(entry.emoji)) {
				if (auto webp = RasterizeNativeEmoji(entry.emoji)
					; !webp.isEmpty()) {
					icon = CustomEmojiDocument(
						session,
						state,
						entry.id,
						entry.emoji,
						webp,
						u"image/webp"_q);
				}
			}
		}
		if (!icon) {
			// A missing or still downloading asset is excluded from the
			// selector; a reaction already present on a message falls back
			// to the placeholder rendering upstream applies by itself, and
			// the entry joins the list on the refresh the download triggers.
			continue;
		}
		result.push_back(Data::Reaction{
			.id = Data::ReactionId{ entry.id },
			.title = entry.emoji,
			.appearAnimation = icon,
			.selectAnimation = icon,
			// centerIcon stays empty on purpose. Upstream reads it only on
			// the plain-emoji paths (Reactions::resolveImageFor, and the
			// non-custom branch of ReactionFlyAnimation); a custom reaction
			// draws its center from the custom emoji itself.
			//
			// aroundAnimation stays empty too: it is the particle burst
			// drawn at twice the icon size around the emoji, not the emoji
			// itself, and fxl-api has no such asset. Reusing the reaction
			// here would paint a doubled copy of it behind the icon.
			.active = true,
		});
	}
	return result;
}

void ApplyDefault(
		not_null<Main::Session*> session,
		not_null<Data::Reactions*> reactions) {
	// Friend-seam implementation: mirrors the shape of the upstream
	// updateDefault() without touching its MTProto request lifecycle. The
	// defaultUpdated() body is deliberately not called: it would fire
	// GetTopReactions/GetRecentReactions over MTProto, which has no session
	// under the bridge. Only the catalog-driven parts run.
	if (reactions->_defaultRequestId) {
		// Upstream's own re-entrancy guard, reused as a "already scheduled"
		// marker: nothing ever cancels this request id.
		return;
	}
	reactions->_defaultRequestId = 1;

	// Data::Reactions is constructed from the Data::Session constructor, so
	// Main::Session::data() is not assigned yet and building the sticker
	// documents would dereference it. The work waits for the next main loop
	// iteration, exactly like the MTProto reply upstream applies.
	crl::on_main(session.get(), [=] {
		auto &self = *reactions;
		self._defaultRequestId = 0;
		self._defaultHash = 0;

		const auto oldCache = base::take(self._iconsCache);
		const auto toCache = [&](DocumentData *document) {
			if (document) {
				self._iconsCache.emplace(document, document->createMediaView());
			}
		};
		const auto list = BuildAvailableReactions(session);
		self._active.clear();
		self._available.clear();
		self._active.reserve(list.size());
		self._available.reserve(list.size());
		for (const auto &reaction : list) {
			self._available.push_back(reaction);
			self._active.push_back(reaction);
			toCache(reaction.appearAnimation);
			toCache(reaction.selectAnimation);
		}
		// The usage orders are resolved against the list just built, not
		// against the catalog: a row whose asset never arrived has no
		// reaction to point at, and a dangling entry here would be a hole at
		// the front of the picker.
		const auto resolveUsage = [&](
				const std::vector<DocumentId> &usage,
				std::vector<Data::ReactionId> &ids,
				std::vector<Data::Reaction> &into) {
			ids.clear();
			into.clear();
			ids.reserve(usage.size());
			into.reserve(usage.size());
			for (const auto emojiId : usage) {
				const auto wanted = Data::ReactionId{ emojiId };
				for (const auto &reaction : list) {
					if (reaction.id == wanted) {
						ids.push_back(reaction.id);
						into.push_back(reaction);
						break;
					}
				}
			}
		};
		const auto &state = StateFor(session);
		resolveUsage(state.topUsage, self._topIds, self._top);
		resolveUsage(state.recentUsage, self._recentIds, self._recent);
		if (self._waitingForReactions) {
			self._waitingForReactions = false;
			self.resolveReactionImages();
		}
		self._defaultUpdated.fire({});
		// Fired directly, not through recentUpdated(): that one arms the
		// upstream top-refresh timer, whose request has no transport here and
		// would sit in MTProto forever. The strip merges all three streams
		// (history_view_reactions_button.cpp), so the order change reaches an
		// already open chat.
		self._topUpdated.fire({});
		self._recentUpdated.fire({});
	});
}

void ClearSession(not_null<Main::Session*> session) {
	// The documents and media views belong to the Data::Session going away,
	// so they go with it - exactly as they would inside upstream's
	// Data::Reactions. Other accounts keep theirs untouched.
	States().remove(session);
}

void SendChosen(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	auto emojiIds = std::vector<DocumentId>();
	for (const auto &id : item->chosenReactions()) {
		// The bridge protocol speaks catalog ids, and a custom-emoji reaction
		// carries one directly: its DocumentId is that id. A plain emoji
		// reaction names no catalog row and is dropped - the catalog serves
		// none of those.
		if (const auto emojiId = id.custom()) {
			emojiIds.push_back(emojiId);
		}
	}
	if (const auto bridge = CustomBackend::BridgeFor(session)) {
		bridge->setReactions(item, emojiIds);
	}
}

QString AssetUrlFor(not_null<Main::Session*> session, DocumentId id) {
    if (!id) {
        return QString();
    }
    for (const auto &entry : StateFor(session).catalog) {
        if (entry.id == id) {
            return entry.assetUrl;
        }
    }
    return QString();
}

QString EmojiFor(not_null<Main::Session*> session, DocumentId id) {
    const auto &map = StateFor(session).emojiByDocumentId;
    const auto i = map.find(id);
    return (i == map.end()) ? QString() : i->second;
}

Asset AssetFor(not_null<Main::Session*> session, DocumentId id) {
    const auto &assets = StateFor(session).assets;
    const auto i = assets.find(id);
    return (i == assets.end()) ? Asset() : i->second;
}

bool IsSourceWebm(not_null<Main::Session*> session, DocumentId id) {
    const auto state = FindState(session);
    return state && state->sourceWebmIds.contains(id);
}

rpl::producer<DocumentId> AssetLoaded(not_null<Main::Session*> session) {
    return StateFor(session).assetLoaded.events();
}

rpl::producer<> CatalogChanged(not_null<Main::Session*> session) {
    return StateFor(session).catalogChanged.events();
}

std::vector<CatalogItem> Catalog(not_null<Main::Session*> session) {
    return StateFor(session).catalog;
}

} // namespace CustomBackend::Reactions
