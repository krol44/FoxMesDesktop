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

struct PendingAsset final {
    QByteArray source;
    QByteArray animated;
    bool sourceDone = false;
    bool animatedDone = false;
};

struct State final {
    std::vector<CatalogItem> catalog;
    int maxSelected = 3;

    std::vector<DocumentId> topUsage;
    std::vector<DocumentId> recentUsage;

    base::flat_map<DocumentId, QString> emojiByDocumentId;

    base::flat_map<DocumentId, std::shared_ptr<Data::DocumentMedia>> media;
    base::flat_map<DocumentId, not_null<DocumentData*>> documents;

    base::flat_map<DocumentId, Asset> assets;
    base::flat_map<DocumentId, PendingAsset> pending;
    QSet<DocumentId> sourceWebmIds;
    QSet<QString> requested;

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

[[nodiscard]] State *FindState(Main::Session *session) {
    const auto i = session ? States().find(session) : States().end();
    return (i == States().end()) ? nullptr : i->second.get();
}

[[nodiscard]] QByteArray RasterizeNativeEmoji(const QString &emoji) {
    const auto found = Ui::Emoji::Find(emoji);
    if (!found) {
        return {};
    }
    const auto images = Ui::Emoji::SourceImages();
    if (!images || !images->ensureLoaded()) {
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
        uint64(0),
        QByteArray(),
        base::unixtime::now(),
        attributes,
        mime,
        InlineImageLocation(),
        ImageWithLocation(),
        ImageWithLocation(),
        false,
        0,
        int64(content.size()));
    if (!document->sticker()) {
        return nullptr;
    }
    auto media = document->createMediaView();
    media->setBytes(content);
    state.media[id] = std::move(media);
    state.documents.insert_or_assign(id, document);
    session->data().customEmojiManager().resolveLocalDocument(document);
    return document;
}

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
        const auto webm = (ok && contentType.startsWith(u"video/webm"_q))
            ? body
            : QByteArray();
        const auto i = state.pending.find(id);
        if (i != state.pending.end()) {
            if (!webm.isEmpty()) {
                i->second.animated = webm;
            }
            i->second.animatedDone = true;
            BuildDownloadedAsset(session, state, id, emoji);
        } else if (!webm.isEmpty()) {
            const auto mime = u"video/webm"_q;
            if (CustomEmojiDocument(session, state, id, emoji, webm, mime)) {
                AssetBuilt(
                    session,
                    state,
                    id,
                    { .content = webm, .mime = mime });
            }
        }
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
			continue;
		}
		result.push_back(Data::Reaction{
			.id = Data::ReactionId{ entry.id },
			.title = entry.emoji,
			.appearAnimation = icon,
			.selectAnimation = icon,
			.active = true,
		});
	}
	return result;
}

void ApplyDefault(
		not_null<Main::Session*> session,
		not_null<Data::Reactions*> reactions) {
	if (reactions->_defaultRequestId) {
		return;
	}
	reactions->_defaultRequestId = 1;

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
		self._topUpdated.fire({});
		self._recentUpdated.fire({});
	});
}

void ClearSession(not_null<Main::Session*> session) {
	States().remove(session);
}

void SendChosen(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	auto emojiIds = std::vector<DocumentId>();
	for (const auto &id : item->chosenReactions()) {
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

}
