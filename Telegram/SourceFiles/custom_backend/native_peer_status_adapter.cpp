#include "custom_backend/native_peer_status_adapter.h"
#include "custom_backend/peer_status_dto.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_reactions_adapter.h"
#include "base/flat_map.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "data/data_emoji_statuses.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <deque>

namespace CustomBackend::PeerStatuses {
namespace {

constexpr auto kStatusSetId = uint64(1) << 49;

struct Mutation {
    qint64 chatId = 0;
    DocumentId documentId = 0;
    TimeId until = 0;
    Mtp::Answer answer;
};
struct State {
    base::flat_map<PeerId, QJsonObject> peers;
    base::flat_map<DocumentId, QJsonObject> assets;
    QJsonArray catalog;
    std::deque<Mutation> mutations;
};
base::flat_map<Main::Session*, std::unique_ptr<State>> states;

State &Get(not_null<Main::Session*> session) {
    auto &slot = states[session];
    if (!slot) slot = std::make_unique<State>();
    return *slot;
}

qint64 Number(const QJsonObject &value, const char *key) {
    return value.value(QLatin1String(key)).toVariant().toLongLong();
}

void RememberAsset(not_null<Main::Session*> session, const QJsonObject &asset) {
    const auto dto = StatusAssetDto::Parse(asset);
    const auto id = DocumentId(dto.documentId);
    if (!id) return;
    Get(session).assets[id] = asset;
    Reactions::RegisterStatusAsset(session, id, dto.url);
}

void LoadCatalog(not_null<Main::Session*> session, Fn<void(bool)> done) {
    if (!Available(session)) { done(false); return; }
    const auto weak = base::make_weak(session);
    const auto endpoint = ClientFor(session).baseUrl();
    ClientFor(session).communityRequest("GET", u"/emoji-statuses"_q, {}, [weak, endpoint, done](QJsonDocument doc, QString error, int code) {
        if (!weak || !BridgeFor(weak.get()) || !Available(weak.get())
            || ClientFor(weak.get()).baseUrl() != endpoint
            || !error.isEmpty() || code != 200) {
            done(false);
            return;
        }
        auto &state = Get(weak.get());
        state.catalog = doc.object().value("items").toArray();
        for (const auto &item : state.catalog) RememberAsset(weak.get(), item.toObject());
        done(true);
    });
}

PeerId MutationPeer(not_null<Main::Session*> session, qint64 chatId) {
    return chatId ? peerFromChannel(ChannelId(chatId)) : session->user()->id;
}

QString StatusPath(qint64 chatId) {
    return chatId ? u"/chats/%1/emoji-status"_q.arg(chatId) : u"/me/emoji-status"_q;
}

void Run(not_null<Main::Session*> session);

void Submit(not_null<Main::Session*> session) {
    auto &state = Get(session);
    if (state.mutations.empty()) return;
    const auto mutation = state.mutations.front();
    const auto peerId = MutationPeer(session, mutation.chatId);
    const auto status = state.peers[peerId].value("emoji_status").toObject();
    const auto body = QJsonObject{
        { "asset_id", mutation.documentId ? QJsonValue(qint64(mutation.documentId)) : QJsonValue(QJsonValue::Null) },
        { "until", mutation.until ? QJsonValue(qint64(mutation.until)) : QJsonValue(QJsonValue::Null) },
        { "expected_revision", Number(status, "revision") },
    };
    const auto weak = base::make_weak(session);
    ClientFor(session).peerStatusRequest("PUT", StatusPath(mutation.chatId), body,
        [weak, mutation, peerId](QJsonDocument doc, QString error, int code) {
            if (!weak || !BridgeFor(weak.get())) return;
            auto &state = Get(weak.get());
            if (state.mutations.empty()) return;
            const auto success = error.isEmpty() && code == 200 && doc.object().contains("revision");
            const auto canonical = success ? doc.object() : doc.object().value("state").toObject();
            if (!canonical.isEmpty()) Merge(weak.get(), peerId, QJsonObject{ { "emoji_status", canonical } });
            state.mutations.pop_front();
            const auto pending = ranges::any_of(state.mutations, [chatId = mutation.chatId](const Mutation &other) { return other.chatId == chatId; });
            if (success) {
                if (!pending) Apply(weak.get(), weak.get()->data().peer(peerId), state.peers[peerId]);
                if (mutation.chatId) mutation.answer.done(MTP_updates(MTPVector<MTPUpdate>(), MTPVector<MTPUser>(), MTPVector<MTPChat>(), MTP_int(base::unixtime::now()), MTP_int(0)));
                else mutation.answer.done(MTP_boolTrue());
                Run(weak.get());
            } else {
                ClientFor(weak.get()).communityRequest("GET", StatusPath(mutation.chatId), {},
                    [weak, mutation, peerId](QJsonDocument doc, QString error, int code) {
                        if (!weak || !BridgeFor(weak.get())) return;
                        auto &state = Get(weak.get());
                        if (error.isEmpty() && code == 200) {
                            Merge(weak.get(), peerId, QJsonObject{ { "emoji_status", doc.object() } });
                        }
                        if (!ranges::any_of(state.mutations, [chatId = mutation.chatId](const Mutation &other) { return other.chatId == chatId; })) {
                            Apply(weak.get(), weak.get()->data().peer(peerId), state.peers[peerId]);
                        }
                        mutation.answer.fail(u"EMOJI_STATUS_SAVE_FAILED"_q, 400);
                        Run(weak.get());
                    });
            }
        });
}

void Run(not_null<Main::Session*> session) {
    auto &state = Get(session);
    if (state.mutations.empty()) return;
    const auto mutation = state.mutations.front();
    const auto peerId = MutationPeer(session, mutation.chatId);
    if (state.peers[peerId].contains("emoji_status")) { Submit(session); return; }
    const auto weak = base::make_weak(session);
    ClientFor(session).communityRequest("GET", StatusPath(mutation.chatId), {}, [weak, peerId, mutation](QJsonDocument doc, QString error, int code) {
        if (!weak || !BridgeFor(weak.get())) return;
        if (Get(weak.get()).mutations.empty()) return;
        if (error.isEmpty() && code == 200) {
            Merge(weak.get(), peerId, QJsonObject{ { "emoji_status", doc.object() } });
            Submit(weak.get());
        } else {
            auto &state = Get(weak.get());
            state.mutations.pop_front();
            mutation.answer.fail(u"EMOJI_STATUS_UNAVAILABLE"_q, 500);
            Run(weak.get());
        }
    });
}

}

bool Available(Main::Session *session) {
    if (!Enabled() || !session) return false;
    for (const auto &value : CurrentUser(session).value("capabilities").toArray()) {
        if (value.toString() == u"emoji-status-v1"_q) return true;
    }
    return false;
}

QJsonObject Merge(not_null<Main::Session*> session, PeerId peer, const QJsonObject &value) {
    auto &current = Get(session).peers[peer];
    if (value.contains("verified") && (!current.contains("verified") || Number(value, "verification_revision") >= Number(current, "verification_revision"))) {
        current.insert("verified", value.value("verified"));
        current.insert("verification_revision", value.value("verification_revision"));
    }
    if (value.contains("emoji_status")) {
        const auto incoming = value.value("emoji_status").toObject();
        if (!current.contains("emoji_status") || Number(incoming, "revision") >= Number(current.value("emoji_status").toObject(), "revision")) {
            current.insert("emoji_status", incoming);
            RememberAsset(session, incoming.value("asset").toObject());
        }
    }
    auto result = value;
    for (auto i = current.begin(); i != current.end(); ++i) result.insert(i.key(), i.value());
    return result;
}

MTPEmojiStatus MapStatus(const EmojiStatusDto &status) {
    const auto id = status.asset ? status.asset->documentId : 0;
    const auto until = status.until;
    if (!id || (until && until <= base::unixtime::now())) return MTP_emojiStatusEmpty();
    return MTP_emojiStatus(MTP_flags(until ? MTPDemojiStatus::Flag::f_until : MTPDemojiStatus::Flag()), MTP_long(id), MTP_int(until));
}

MTPEmojiStatus NativeStatus(const QJsonObject &value) {
    const auto dto = PeerDecorationDto::Parse(value);
    return dto.status ? MapStatus(*dto.status) : MTP_emojiStatusEmpty();
}

void Apply(not_null<Main::Session*> session, not_null<PeerData*> peer, const QJsonObject &value) {
    const auto merged = Merge(session, peer->id, value);
    const auto dto = PeerDecorationDto::Parse(merged);
    if (dto.verified.has_value()) {
        const auto verified = *dto.verified;
        if (const auto user = peer->asUser()) {
            if (verified) user->addFlags(UserDataFlag::Verified);
            else user->removeFlags(UserDataFlag::Verified);
        } else if (const auto channel = peer->asChannel()) {
            if (verified) channel->addFlags(ChannelDataFlag::Verified);
            else channel->removeFlags(ChannelDataFlag::Verified);
        }
    }
    if (dto.status && !ranges::any_of(Get(session).mutations, [session, peer](const Mutation &mutation) {
        return MutationPeer(session, mutation.chatId) == peer->id;
    })) peer->setEmojiStatus(MapStatus(*dto.status));
}

void ApplyUser(not_null<Main::Session*> session, const QJsonObject &value) {
    const auto id = PeerDecorationDto::Parse(value).userId;
    if (id > 0) Apply(session, session->data().user(UserId(id)), value);
}

void Refresh(not_null<Main::Session*> session) {
    if (!Available(session)) return;
    LoadCatalog(session, [weak = base::make_weak(session)](bool ok) {
        if (!weak || !ok) return;
        weak.get()->data().emojiStatuses().refreshDefault();
        weak.get()->data().emojiStatuses().refreshColored();
        weak.get()->data().emojiStatuses().refreshRecent();
        weak.get()->data().emojiStatuses().refreshChannelDefault();
    });
}

bool ApplyEvent(not_null<Main::Session*> session, const QString &type, const QJsonObject &value) {
    if (type != u"chat.decoration.updated"_q) return false;
    const auto id = PeerDecorationDto::Parse(value).chatId;
    if (id > 0) {
        if (const auto bridge = BridgeFor(session)) {
            auto chat = bridge->communityChat(id);
            if (!chat.isEmpty()) {
                for (auto i = value.begin(); i != value.end(); ++i) if (i.key() != u"chat_id"_q) chat.insert(i.key(), i.value());
                bridge->applyCommunityChat(chat);
            } else Apply(session, session->data().channel(ChannelId(id)), value);
        }
    }
    return true;
}

bool Intercepts(const MTP::details::SerializedRequest &request) {
    const auto type = Mtp::RequestType(request);
    if (type == mtpc_account_updateEmojiStatus || type == mtpc_channels_updateEmojiStatus
        || type == mtpc_account_getDefaultEmojiStatuses || type == mtpc_account_getRecentEmojiStatuses
        || type == mtpc_account_clearRecentEmojiStatuses || type == mtpc_account_getCollectibleEmojiStatuses) return true;
    if (type == mtpc_messages_getStickerSet) {
        auto reader = Mtp::Reader(request);
        const auto set = reader.read<MTPInputStickerSet>();
        return set.type() == mtpc_inputStickerSetEmojiDefaultStatuses || set.type() == mtpc_inputStickerSetEmojiChannelDefaultStatuses;
    }
    return false;
}

void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId, const MTP::details::SerializedRequest &request) {
    const auto answer = Mtp::Answer(instance, requestId);
    const auto type = Mtp::RequestType(request);
    auto reader = Mtp::Reader(request);
    auto chatId = qint64(0);
    auto chosen = MTPEmojiStatus();
    if (type == mtpc_channels_updateEmojiStatus) {
        const auto channel = reader.read<MTPInputChannel>();
        if (channel.type() == mtpc_inputChannel) chatId = channel.c_inputChannel().vchannel_id().v;
        if (!chatId) { answer.fail(u"CHANNEL_INVALID"_q); return; }
    }
    if (type == mtpc_account_updateEmojiStatus || type == mtpc_channels_updateEmojiStatus) {
        chosen = reader.read<MTPEmojiStatus>();
        if (chosen.type() != mtpc_emojiStatus && chosen.type() != mtpc_emojiStatusEmpty) { answer.fail(u"EMOJI_STATUS_INVALID"_q); return; }
    }
    if (!reader.ok()) { answer.fail(u"EMOJI_STATUS_INVALID"_q); return; }
    crl::on_main([instance = QPointer<MTP::Instance>(instance.get()), answer, type, chosen, chatId] {
        if (!instance) return;
        const auto session = Mtp::SessionFor(instance.data());
        if (!session) { answer.fail(u"SESSION_REVOKED"_q, 401); return; }
        if (type == mtpc_account_updateEmojiStatus || type == mtpc_channels_updateEmojiStatus) {
            auto id = DocumentId();
            auto until = TimeId();
            if (chosen.type() == mtpc_emojiStatus) {
                const auto &data = chosen.c_emojiStatus();
                id = DocumentId(data.vdocument_id().v);
                until = TimeId(data.vuntil().value_or_empty());
            }
            auto &queue = Get(session).mutations;
            queue.push_back({ chatId, id, until, answer });
            if (queue.size() == 1) Run(session);
        } else if (type == mtpc_account_clearRecentEmojiStatuses) {
            answer.done(MTP_boolTrue());
        } else if (type == mtpc_account_getRecentEmojiStatuses) {
            answer.done(MTP_account_emojiStatuses(MTP_long(0), MTPVector<MTPEmojiStatus>()));
        } else if (type == mtpc_account_getCollectibleEmojiStatuses) {
            answer.done(MTP_account_emojiStatuses(MTP_long(0), MTPVector<MTPEmojiStatus>()));
        } else {
            LoadCatalog(session, [weak = base::make_weak(session), answer, type](bool ok) {
                if (!weak) { answer.fail(u"SESSION_REVOKED"_q, 401); return; }
                if (!ok) { answer.fail(u"EMOJI_STATUS_CATALOG_UNAVAILABLE"_q, 500); return; }
                auto statuses = QVector<MTPEmojiStatus>();
                for (const auto &item : Get(weak.get()).catalog) {
                    const auto asset = StatusAssetDto::Parse(item.toObject());
                    statuses.push_back(MTP_emojiStatus(MTP_flags(0), MTP_long(asset.documentId), MTPint()));
                }
                if (type == mtpc_messages_getStickerSet) {
                    // Status documents come from the reactions adapter once their files are downloaded.
                    using F = MTPDstickerSet::Flag;
                    const auto set = MTP_stickerSet(MTP_flags(F::f_emojis), MTPint(), MTP_long(kStatusSetId), MTP_long(0),
                        MTP_string(u"Emoji statuses"_q), MTPstring(), MTPVector<MTPPhotoSize>(), MTPint(), MTPint(), MTPlong(), MTP_int(0), MTP_int(1));
                    answer.done(MTP_messages_stickerSet(set, MTPVector<MTPStickerPack>(), MTPVector<MTPStickerKeyword>(), MTPVector<MTPDocument>()));
                } else answer.done(MTP_account_emojiStatuses(MTP_long(0), MTP_vector<MTPEmojiStatus>(statuses)));
            });
        }
    });
}

void ClearSession(not_null<Main::Session*> session) { states.remove(session.get()); }

}
