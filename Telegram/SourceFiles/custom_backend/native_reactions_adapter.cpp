#include "custom_backend/native_reactions_adapter.h"

#include "base/flat_map.h"
#include "base/unixtime.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/api_client.h"
#include "custom_backend/default_reaction_state.h"
#include "mtproto/mtp_instance.h"
#include <QUuid>
#include "custom_backend/native_runtime.h"
#include "history/history_item.h"
#include "data/data_message_reactions.h"
#include "data/data_message_reaction_id.h"
#include "data/data_peer_id.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "ui/emoji_config.h"
#include "ui/text/text_utilities.h"
#include "main/main_session.h"

#include <QHash>
#include <QPointer>
#include <QSet>

namespace CustomBackend::Reactions {
namespace {

struct State final {
    Data::Reactions *reactions = nullptr;
    DefaultReactionState favorite;
    std::vector<Mtp::Answer> favoriteAnswers;
    bool favoriteLoading = false;
    std::vector<CatalogItem> catalog;
    int maxSelected = 3;

    std::vector<DocumentId> topUsage;
    std::vector<DocumentId> recentUsage;

    base::flat_map<DocumentId, QString> emojiByDocumentId;
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

void DispatchFavorite(not_null<Main::Session*> session);

[[nodiscard]] DocumentId EffectiveFavorite(const State &state) {
    auto catalog = std::vector<DefaultReactionState::Entry>();
    for (const auto &entry : state.catalog) catalog.push_back({ entry.id, entry.emoji.toStdString() });
    return state.favorite.effective(catalog);
}

void DispatchFavorite(not_null<Main::Session*> session) {
    auto &state = StateFor(session);
    if (state.favorite.sent || !state.favorite.desired) return;
    if (state.favorite.revision < 0) { RequestFavorite(session); return; }
    const auto mutation = *state.favorite.begin();
    const auto sent = mutation.id;
    auto answers = std::move(state.favoriteAnswers);
    state.favoriteAnswers.clear();
    if (mutation.unchanged) {
        state.favorite.finish(true);
        for (const auto &answer : answers) answer.done(MTP_boolTrue());
        ApplyFavorite(session);
        return;
    }
    const auto body = QJsonObject{
        {"emoji_id", qint64(sent)},
        {"expected_revision", qint64(mutation.revision)},
        {"operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    };
    const auto weak = base::make_weak(session);
    const auto finish = [weak, answers = std::move(answers)](QJsonDocument doc, QString error, int status) {
        const auto session = weak.get();
        const auto state = FindState(session);
        if (!state) return;
        const auto success = error.isEmpty() && status == 200 && doc.object().contains("revision");
        state->favorite.finish(success);
        if (success) ReceiveFavorite(session, doc.object());
        else if (status == 409 && doc.object().value("state").isObject()) ReceiveFavorite(session, doc.object().value("state").toObject());
        ApplyFavorite(session);
        for (const auto &answer : answers) {
            if (success) answer.done(MTP_boolTrue());
            else answer.fail(u"REACTION_INVALID"_q);
        }
        if (!success) {
            state->favorite.desired.reset();
            for (const auto &answer : state->favoriteAnswers) answer.fail(u"REACTION_INVALID"_q);
            state->favoriteAnswers.clear();
            ApplyFavorite(session);
        } else if (state->favorite.desired) DispatchFavorite(session);
        if (!success) RequestFavorite(session);
    };
    ClientFor(session).communityRequest("PUT", u"/reactions/default"_q, body,
        [weak, body, finish](QJsonDocument doc, QString error, int status) {
            if (!weak) return;
            if (status <= 0 || status >= 500) {
                ClientFor(weak.get()).operationResult(body.value("operation_id").toString(),
                    [finish](QJsonDocument recovered, QString lookupError, int lookupStatus) {
                        const auto value = recovered.object();
                        if (lookupError.isEmpty() && value.value("status").toString() == u"done"_q && value.value("result").isObject()) {
                            finish(QJsonDocument(value.value("result").toObject()), {}, 200);
                        } else finish({}, u"NETWORK_ERROR"_q, lookupStatus == 409 ? 409 : 500);
                    });
            } else finish(doc, error, status);
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
    auto &state = StateFor(session);
    for (const auto &entry : entries) {
        state.emojiByDocumentId[entry.id] = entry.emoji;
    }
    state.catalog = std::move(entries);
    state.catalogChanged.fire({});
    ApplyFavorite(session);
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
		if (entry.emoji.isEmpty() || entry.url.isEmpty()) {
			continue;
		}
		const auto icon = StickerSets::Register(session, entry);
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
		ApplyFavorite(session);
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

void ApplyFavorite(not_null<Main::Session*> session) {
    const auto state = FindState(session);
    if (!state || !state->reactions) return;
    auto &reactions = *state->reactions;
    const auto id = EffectiveFavorite(*state);
    reactions.applyFavorite(id ? Data::ReactionId{ id } : Data::ReactionId());
    reactions._favorite = reactions.resolveById(reactions._favoriteId);
    reactions._favoriteUpdated.fire({});
}

void ReceiveFavorite(not_null<Main::Session*> session, const QJsonObject &value) {
    if (!value.value("revision").isDouble() || !value.contains("emoji_id")) return;
    auto &state = StateFor(session);
    const auto revision = value.value("revision").toVariant().toLongLong();
    if (revision < 0 || revision < state.favorite.revision) return;
    state.favorite.receive(value.value("emoji_id").toVariant().toULongLong(), revision);
    if (!state.favorite.sent && !state.favorite.desired) ApplyFavorite(session);
}

void RequestFavorite(not_null<Main::Session*> session) {
    auto &state = StateFor(session);
    if (state.favoriteLoading) return;
    state.favoriteLoading = true;
    const auto weak = base::make_weak(session);
    ClientFor(session).communityRequest("GET", u"/reactions/default"_q, {},
        [weak](QJsonDocument doc, QString error, int status) {
            const auto session = weak.get();
            const auto state = FindState(session);
            if (!state) return;
            state->favoriteLoading = false;
            if (error.isEmpty() && status == 200 && doc.object().value("revision").isDouble()
                    && doc.object().value("revision").toVariant().toLongLong() >= 0
                    && doc.object().contains("emoji_id")) {
                ReceiveFavorite(session, doc.object());
                DispatchFavorite(session);
            } else {
                state->favorite.desired.reset();
                for (const auto &answer : state->favoriteAnswers) answer.fail(u"NETWORK_ERROR"_q, 500);
                state->favoriteAnswers.clear();
                ApplyFavorite(session);
            }
        });
}

void BindFavorite(not_null<Main::Session*> session, not_null<Data::Reactions*> reactions) {
    StateFor(session).reactions = reactions;
    ApplyFavorite(session);
    RequestFavorite(session);
}

bool Intercepts(const MTP::details::SerializedRequest &request) {
    return Mtp::RequestType(request) == mtpc_messages_setDefaultReaction;
}

void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId, const MTP::details::SerializedRequest &request) {
    const auto answer = Mtp::Answer(instance, requestId);
    auto reader = Mtp::Reader(request);
    const auto reaction = reader.read<MTPReaction>();
    if (!reader.ok() || reaction.type() != mtpc_reactionCustomEmoji) { answer.fail(u"REACTION_INVALID"_q); return; }
    const auto id = DocumentId(reaction.c_reactionCustomEmoji().vdocument_id().v);
    crl::on_main([instance = QPointer<MTP::Instance>(instance.get()), answer, id] {
        if (!instance) return;
        const auto session = Mtp::SessionFor(instance.data());
        if (!session) { answer.fail(u"SESSION_REVOKED"_q, 401); return; }
        auto &state = StateFor(session);
        if (!id) { answer.fail(u"REACTION_INVALID"_q); return; }
        state.favorite.desired = id;
        state.favoriteAnswers.push_back(answer);
        DispatchFavorite(session);
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
            return entry.url;
        }
    }
    const auto item = StickerSets::Find(session, id);
    return item ? item->url : QString();
}

QString EmojiFor(not_null<Main::Session*> session, DocumentId id) {
    const auto &map = StateFor(session).emojiByDocumentId;
    const auto i = map.find(id);
    return (i == map.end()) ? QString() : i->second;
}

rpl::producer<> CatalogChanged(not_null<Main::Session*> session) {
    return StateFor(session).catalogChanged.events();
}

std::vector<CatalogItem> Catalog(not_null<Main::Session*> session) {
    return StateFor(session).catalog;
}

}
