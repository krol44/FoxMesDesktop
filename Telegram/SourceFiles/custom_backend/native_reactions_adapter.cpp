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

// One catalog entry as served by GET /reactions v2: the emoji alt plus the
// CDN asset used only when the client cannot render the alt natively.
struct CatalogEntry final {
    QString emoji;
    QString assetUrl;
    // The emojis.category_name of the entry. Reactions ignore it; the stickers
    // panel groups its sets by it, which is the only grouping the catalog has.
    QString category;
};

QVector<CatalogEntry> &CatalogStorage() {
    static auto values = QVector<CatalogEntry>{
        { u"👍"_q, QString() },
        { u"❤️"_q, QString() },
        { u"😂"_q, QString() },
        { u"🔥"_q, QString() },
        { u"😢"_q, QString() },
        { u"😡"_q, QString() },
        { u"👏"_q, QString() },
        { u"🎉"_q, QString() },
    };
    return values;
}

QStringList &AvailableEmojiStorage() {
    static auto values = QStringList{
        u"👍"_q,
        u"❤️"_q,
        u"😂"_q,
        u"🔥"_q,
        u"😢"_q,
        u"😡"_q,
        u"👏"_q,
        u"🎉"_q,
    };
    return values;
}

int &MaxSelectedStorage() {
    static auto value = 3;
    return value;
}

constexpr auto kStaticEmojiSize = 100;

// Bridge reactions are custom-emoji reactions: upstream identifies them by
// DocumentId, not by the emoji string, and that is what routes the inline
// badge through Ui::Text::CustomEmoji instead of the single cached frame
// Reactions::resolveImageFor() bakes for plain emoji reactions.
//
// The id is derived from the alt alone, never from the asset url, because
// Build() has to turn an incoming reaction into a ReactionId synchronously,
// long before (or without ever) the asset behind it downloading. Nothing
// server-side depends on the value: it stays inside this client, and
// SendChosen() maps it back to the alt through the document's sticker alt.
[[nodiscard]] DocumentId DocumentIdForEmoji(const QString &emoji) {
    auto full = u"foxmes_custom_reaction:"_q;
    full.append(emoji);
    return qHash(full);
}

// Reverse of DocumentIdForEmoji(), for the ids the client hands back to the
// bridge. It is filled from the same call, so any id that ever reached
// upstream can be resolved here without touching Data::Session.
base::flat_map<DocumentId, QString> &EmojiByDocumentId() {
    static auto value = base::flat_map<DocumentId, QString>();
    return value;
}

[[nodiscard]] DocumentId RegisterEmoji(const QString &emoji) {
    const auto id = DocumentIdForEmoji(emoji);
    EmojiByDocumentId()[id] = emoji;
    return id;
}

[[nodiscard]] QString EmojiForDocumentId(DocumentId id) {
    const auto &map = EmojiByDocumentId();
    const auto i = map.find(id);
    return (i == map.end()) ? QString() : i->second;
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

// The media view that owns each reaction document's bytes, keyed by alt like
// DocumentCache(). A reaction document exists only in memory: nothing
// uploads or downloads it, so the only copy of its content is the one held
// here.
base::flat_map<QString, std::shared_ptr<Data::DocumentMedia>> &MediaCache() {
    static base::flat_map<QString, std::shared_ptr<Data::DocumentMedia>> value;
    return value;
}

// Documents already built for this session, keyed by alt. The cache is what
// makes repeated selector builds cheap.
base::flat_map<QString, DocumentData*> &DocumentCache() {
    static auto value = base::flat_map<QString, DocumentData*>();
    return value;
}

// The asset bytes behind each alt. Kept so the stickers panel can build its
// own documents from them: it needs the same picture carrying sticker
// attributes instead of custom-emoji ones, and downloading the whole catalog a
// second time for that would double the traffic of every login.
base::flat_map<QString, Asset> &AssetCache() {
    static auto value = base::flat_map<QString, Asset>();
    return value;
}

// Fires once per alt whose asset just landed, so a panel built before the
// download finished can rebuild instead of staying empty until the next
// catalog refresh.
rpl::event_stream<QString> &AssetLoadedStream() {
    static auto value = rpl::event_stream<QString>();
    return value;
}

// Fires when the catalog itself is replaced.
rpl::event_stream<> &CatalogChangedStream() {
    static auto value = rpl::event_stream<>();
    return value;
}

// Builds the in-memory custom-emoji document behind one reaction.
//
// documentAttributeCustomEmoji, not documentAttributeSticker: it is what
// makes DocumentData::setattributes() write StickersType::Emoji, and the
// free flag is mandatory - without it the same call raises PremiumSticker
// and the whole catalog disappears behind the premium gate.
//
// The alt carries the emoji: upstream maps a custom reaction back to its
// plain counterpart through sticker()->alt (Reactions::chooseGenericAnimation,
// Reactions::preloadImageFor), and SendChosen() reads it for the same reason.
[[nodiscard]] DocumentData *CustomEmojiDocument(
        not_null<Main::Session*> session,
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
        RegisterEmoji(emoji),
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
    MediaCache()[emoji] = std::move(media);
    DocumentCache()[emoji] = document;
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
void FetchAsset(
        const QString &url,
        Fn<bool(
            bool ok,
            const QByteArray &body,
            const QString &contentType)> apply) {
    static QSet<QString> requested;
    if (requested.contains(url)) {
        return;
    }
    requested.insert(url);
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
    QObject::connect(reply, &QNetworkReply::finished, reply, [url, reply, apply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (!apply(false, {}, {})) {
                requested.remove(url);
            }
            return;
        }
        const auto contentType = reply->header(
            QNetworkRequest::ContentTypeHeader).toString();
        if (!apply(true, reply->readAll(), contentType)) {
            requested.remove(url);
        }
    });
}

void RefreshReactions(base::weak_ptr<Main::Session> weakSession) {
    crl::on_main(weakSession, [weakSession] {
        if (const auto strong = weakSession.get()) {
            strong->data().reactions().refreshDefault();
        }
    });
}

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

base::flat_map<QString, PendingAsset> &PendingAssets() {
    static auto value = base::flat_map<QString, PendingAsset>();
    return value;
}

void BuildDownloadedAsset(
        base::weak_ptr<Main::Session> weakSession,
        const QString &emoji) {
    const auto i = PendingAssets().find(emoji);
    if (i == PendingAssets().end()
        || !i->second.sourceDone
        || !i->second.animatedDone) {
        return;
    }
    const auto pending = i->second;
    PendingAssets().erase(i);
    const auto session = weakSession.get();
    if (!session) {
        return;
    }
    const auto animated = !pending.animated.isEmpty();
    const auto content = animated ? pending.animated : pending.source;
    const auto mime = animated ? u"video/webm"_q : u"image/webp"_q;
    const auto built = CustomEmojiDocument(session, emoji, content, mime);
    if (built) {
        AssetCache()[emoji] = Asset{ .content = content, .mime = mime };
        RefreshReactions(weakSession);
        AssetLoadedStream().fire_copy(emoji);
    }
}

void DownloadAsset(
        base::weak_ptr<Main::Session> weakSession,
        const QString &emoji,
        const QString &url) {
    // Insert-if-absent: a request already in flight keeps its own record.
    if (!PendingAssets().contains(emoji)) {
        PendingAssets().emplace(emoji, PendingAsset());
    }

    FetchAsset(url, [weakSession, emoji](
            bool ok,
            const QByteArray &body,
            const QString&) {
        const auto normalized = ok
            ? NormalizeToSquareWebp(body)
            : QByteArray();
        const auto i = PendingAssets().find(emoji);
        if (i == PendingAssets().end()) {
            return ok;
        }
        i->second.source = normalized;
        i->second.sourceDone = true;
        BuildDownloadedAsset(weakSession, emoji);
        return ok && !normalized.isEmpty();
    });

    FetchAsset(AnimatedUrl(url), [weakSession, emoji](
            bool ok,
            const QByteArray &body,
            const QString &contentType) {
        // A source without animation comes back echoed as the original, so
        // only the transcoded mime marks a usable rendition.
        const auto webm = (ok && contentType.startsWith(u"video/webm"_q))
            ? body
            : QByteArray();
        const auto i = PendingAssets().find(emoji);
        if (i != PendingAssets().end()) {
            i->second.animated = webm;
            i->second.animatedDone = true;
            BuildDownloadedAsset(weakSession, emoji);
        } else if (ok && !webm.isEmpty()) {
            // A retry that finally produced the animation: rebuild over the
            // still that was put in place while this request kept failing.
            if (const auto session = weakSession.get()) {
                if (CustomEmojiDocument(
                        session,
                        emoji,
                        webm,
                        u"video/webm"_q)) {
                    // Same bookkeeping as the first-time build: the stickers
                    // panel is holding the still that was put in place while
                    // this request kept failing, and only this tells it there
                    // is an animation now.
                    AssetCache()[emoji] = Asset{
                        .content = webm,
                        .mime = u"video/webm"_q,
                    };
                    RefreshReactions(weakSession);
                    AssetLoadedStream().fire_copy(emoji);
                }
            }
        }
        // A transport error is worth another attempt; an echoed original is
        // the CDN's final answer for this alt.
        return ok;
    });
}

struct Aggregate final {
    int count = 0;
    bool chosen = false;
    QVector<qint64> recentUserIds;
};

QVector<MTPReactionCount> BuildCounts(
        const QHash<QString, Aggregate> &aggregates,
        const QStringList &order) {
    auto result = QVector<MTPReactionCount>();
    auto chosenOrder = 0;
    for (const auto &emoji : order) {
        const auto i = aggregates.find(emoji);
        if (i == aggregates.end() || emoji.isEmpty() || i->count <= 0) {
            continue;
        }
        using Flag = MTPDreactionCount::Flag;
        const auto flags = i->chosen ? Flag::f_chosen_order : Flag();
        result.push_back(MTP_reactionCount(
            MTP_flags(flags),
            i->chosen ? MTP_int(++chosenOrder) : MTPint(),
            Data::ReactionToMTP(Data::ReactionId{ RegisterEmoji(emoji) }),
            MTP_int(i->count)));
    }
    return result;
}

QVector<MTPMessagePeerReaction> BuildRecent(
        const QHash<QString, Aggregate> &aggregates,
        const QStringList &order,
        qint64 myUserId) {
    auto result = QVector<MTPMessagePeerReaction>();
    const auto now = base::unixtime::now();
    for (const auto &emoji : order) {
        const auto i = aggregates.find(emoji);
        if (i == aggregates.end() || emoji.isEmpty()) {
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
                Data::ReactionToMTP(Data::ReactionId{ RegisterEmoji(emoji) })));
        }
    }
    return result;
}

QPair<QHash<QString, Aggregate>, QStringList> Normalize(
        const QJsonArray &reactions,
        qint64 myUserId) {
    auto aggregates = QHash<QString, Aggregate>();
    auto order = QStringList();
    for (const auto &entry : reactions) {
        const auto object = entry.toObject();
        const auto emoji = object.value("emoji").toString();
        if (emoji.isEmpty()) {
            continue;
        }
        if (!aggregates.contains(emoji)) {
            order.push_back(emoji);
        }
        auto &aggregate = aggregates[emoji];
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
    auto flags = Flags(Flag::f_can_see_list);
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
        const QStringList &emojis,
        const QStringList &assetUrls,
        const QStringList &categories) {
    auto entries = QVector<CatalogEntry>();
    for (auto i = 0; i < emojis.size(); ++i) {
        const auto emoji = emojis[i];
        if (emoji.isEmpty()) {
            continue;
        }
        entries.push_back(CatalogEntry{
            .emoji = emoji,
            .assetUrl = i < assetUrls.size() ? assetUrls[i] : QString(),
            .category = i < categories.size() ? categories[i] : QString(),
        });
    }
    if (entries.isEmpty()) {
        return;
    }
    CatalogStorage() = std::move(entries);
    CatalogChangedStream().fire({});
}

int MaxSelectedReactions() {
    return MaxSelectedStorage();
}

void SetMaxSelectedReactions(int value) {
    if (value < 1) {
        return;
    }
    MaxSelectedStorage() = value;
}

std::vector<Data::Reaction> BuildAvailableReactions(Main::Session *session) {
	auto result = std::vector<Data::Reaction>();
	result.reserve(CatalogStorage().size());
	for (const auto &entry : CatalogStorage()) {
		if (entry.emoji.isEmpty()) {
			continue;
		}
		const auto i = DocumentCache().find(entry.emoji);
		auto icon = (i == DocumentCache().end()) ? nullptr : i->second;
		if (!icon && session) {
			if (Ui::Emoji::Find(entry.emoji)) {
				// Native Unicode emoji: always the built-in client graphics.
				if (auto webp = RasterizeNativeEmoji(entry.emoji)
					; !webp.isEmpty()) {
					icon = CustomEmojiDocument(
						session,
						entry.emoji,
						webp,
						u"image/webp"_q);
				}
			} else if (!entry.assetUrl.isEmpty()) {
				// Unknown/custom alt: the CDN asset is the only source.
				DownloadAsset(
					base::make_weak(not_null{ session }),
					entry.emoji,
					entry.assetUrl);
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
			// Custom-emoji reaction: the id is the document, which is what
			// makes InlineList::prepareButtonWithId() build a
			// Ui::Text::CustomEmoji for the inline badge instead of the one
			// static frame Reactions::resolveImageFor() bakes for plain
			// emoji reactions.
			.id = Data::ReactionId{ DocumentIdForEmoji(entry.emoji) },
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
		if (self._waitingForReactions) {
			self._waitingForReactions = false;
			self.resolveReactionImages();
		}
		self._defaultUpdated.fire({});
	});
}

void ClearSessionCaches() {
	// DocumentCache()/MediaCache()/EmojiByDocumentId() are process-lifetime
	// statics, but the DocumentData they point to belongs to the
	// Data::Session that is going away: called from ~Reactions(), i.e. once
	// per session teardown, so the next login rebuilds every entry against
	// its own fresh documents instead of reusing dangling pointers.
	DocumentCache().clear();
	MediaCache().clear();
	EmojiByDocumentId().clear();
	AssetCache().clear();
}

void SendChosen(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	auto emojis = QStringList();
	for (const auto &id : item->chosenReactions()) {
		// The bridge protocol speaks alts, so a custom-emoji reaction is
		// mapped back through the registry that minted its document id. A
		// plain emoji reaction still resolves directly, which is what keeps
		// the non-custom path usable once the catalog serves those again.
		const auto emoji = id.emoji().isEmpty()
			? EmojiForDocumentId(id.custom())
			: id.emoji();
		if (!emoji.isEmpty()) {
			emojis.push_back(emoji);
		}
	}
	if (const auto bridge = CustomBackend::BridgeFor(session)) {
		bridge->setReactions(item, emojis);
	}
}

DocumentId RegisterDocumentId(const QString &emoji) {
    return RegisterEmoji(emoji);
}

QString AssetUrlFor(const QString &emoji) {
    if (emoji.isEmpty()) {
        return QString();
    }
    for (const auto &entry : CatalogStorage()) {
        if (entry.emoji == emoji) {
            return entry.assetUrl;
        }
    }
    return QString();
}

QString EmojiFor(DocumentId id) {
    return EmojiForDocumentId(id);
}

Asset AssetFor(const QString &emoji) {
    const auto &cache = AssetCache();
    const auto i = cache.find(emoji);
    return (i == cache.end()) ? Asset() : i->second;
}

rpl::producer<QString> AssetLoaded() {
    return AssetLoadedStream().events();
}

rpl::producer<> CatalogChanged() {
    return CatalogChangedStream().events();
}

std::vector<CatalogItem> Catalog() {
    auto result = std::vector<CatalogItem>();
    result.reserve(CatalogStorage().size());
    for (const auto &entry : CatalogStorage()) {
        if (entry.emoji.isEmpty()) {
            continue;
        }
        result.push_back(CatalogItem{
            .emoji = entry.emoji,
            .assetUrl = entry.assetUrl,
            .category = entry.category,
        });
    }
    return result;
}

} // namespace CustomBackend::Reactions
