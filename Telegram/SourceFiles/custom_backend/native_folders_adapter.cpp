#include "custom_backend/native_folders_adapter.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "base/flat_map.h"
#include "base/weak_ptr.h"
#include "base/unixtime.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include <QtCore/QUuid>
#include <deque>
#include <set>

namespace CustomBackend::Folders {
namespace {
constexpr auto kSharingEnabled = false;
bool IsSharingRequest(mtpTypeId type) {
    switch (type) {
    case mtpc_chatlists_exportChatlistInvite:
    case mtpc_chatlists_deleteExportedInvite:
    case mtpc_chatlists_editExportedInvite:
    case mtpc_chatlists_getExportedInvites:
    case mtpc_chatlists_checkChatlistInvite:
    case mtpc_chatlists_joinChatlistInvite:
    case mtpc_chatlists_getChatlistUpdates:
    case mtpc_chatlists_joinChatlistUpdates:
    case mtpc_chatlists_hideChatlistUpdates:
    case mtpc_chatlists_getLeaveChatlistSuggestions:
    case mtpc_chatlists_leaveChatlist:
        return true;
    default: return false;
    }
}

struct Folder {
    int id = 0;
    QString title, emoticon;
    QVector<MTPMessageEntity> entities;
    MTPDdialogFilter::Flags flags;
    std::optional<int> color;
    std::vector<qint64> include, pinned, exclude;
};
struct State {
    qint64 revision = -1;
    bool tags = false, loading = false;
    std::vector<int> order = { 0 };
    base::flat_map<int, Folder> folders;
    base::flat_map<QByteArray, qint64> editRevisions;
    base::flat_map<PeerId, qint64> chats;
    base::flat_map<qint64, QJsonObject> chatData;
    std::deque<Fn<void(Fn<void()>)>> queue;
};
base::flat_map<Main::Session*, std::unique_ptr<State>> states;
State &Get(not_null<Main::Session*> session) {
    auto &slot = states[session];
    if (!slot) {
        slot = std::make_unique<State>();
        session->lifetime().add([raw = session.get()] { states.remove(raw); });
    }
    return *slot;
}
std::vector<qint64> Ids(const QJsonArray &array) {
    auto result = std::vector<qint64>();
    for (const auto &value : array) result.push_back(value.toVariant().toLongLong());
    return result;
}
QVector<MTPMessageEntity> Entities(const QJsonArray &array) {
    auto result = QVector<MTPMessageEntity>();
    for (const auto &item : array) {
        const auto object = item.toObject();
        const auto offset = MTP_int(object.value("offset").toInt());
        const auto length = MTP_int(object.value("length").toInt());
        const auto type = object.value("type").toString();
        if (type == u"bold"_q) result.push_back(MTP_messageEntityBold(offset, length));
        else if (type == u"italic"_q) result.push_back(MTP_messageEntityItalic(offset, length));
        else if (type == u"underline"_q) result.push_back(MTP_messageEntityUnderline(offset, length));
        else if (type == u"strikethrough"_q) result.push_back(MTP_messageEntityStrike(offset, length));
        else if (type == u"spoiler"_q) result.push_back(MTP_messageEntitySpoiler(offset, length));
        else if (type == u"custom_emoji"_q) result.push_back(MTP_messageEntityCustomEmoji(offset, length, MTP_long(object.value("emoji_id").toVariant().toLongLong())));
    }
    return result;
}
std::optional<Folder> ParseFolder(const QJsonObject &dto) {
    using F = MTPDdialogFilter::Flag;
    auto folder = Folder();
    folder.id = dto.value("id").toInt(); folder.title = dto.value("title").toString();
    folder.emoticon = dto.value("emoticon").toString(); folder.flags = F::f_emoticon;
    folder.entities = Entities(dto.value("title_entities").toArray());
    if (dto.value("color").isDouble()) { folder.color = dto.value("color").toInt(); folder.flags |= F::f_color; }
    const auto flag = [&](const char *name, F value) { if (dto.value(name).toBool()) folder.flags |= value; };
    flag("contacts", F::f_contacts); flag("non_contacts", F::f_non_contacts); flag("groups", F::f_groups);
    flag("channels", F::f_broadcasts); flag("bots", F::f_bots); flag("exclude_muted", F::f_exclude_muted);
    flag("exclude_read", F::f_exclude_read); flag("exclude_archived", F::f_exclude_archived); flag("static_title", F::f_title_noanimate);
    folder.include = Ids(dto.value("include_chat_ids").toArray()); folder.pinned = Ids(dto.value("pinned_chat_ids").toArray()); folder.exclude = Ids(dto.value("exclude_chat_ids").toArray());
    if (folder.id < 2 || folder.title.trimmed().isEmpty() || folder.title.size() > 32
        || folder.emoticon.toUcs4().size() > 16 || (folder.color && (*folder.color < 0 || *folder.color > 7))
        || folder.include.size() + folder.pinned.size() > 200 || folder.exclude.size() > 200) return std::nullopt;
    auto seen = std::set<qint64>();
    for (const auto *ids : { &folder.include, &folder.pinned, &folder.exclude }) {
        for (const auto id : *ids) if (id <= 0 || !seen.insert(id).second) return std::nullopt;
    }
    const auto entities = dto.value("title_entities").toArray();
    if (entities.size() > 32) return std::nullopt;
    for (const auto &value : entities) {
        const auto entity = value.toObject();
        const auto offset = entity.value("offset").toInt(-1), length = entity.value("length").toInt(-1);
        const auto type = entity.value("type").toString();
        if (offset < 0 || length <= 0 || offset > folder.title.size() - length) return std::nullopt;
        if (type == u"custom_emoji"_q) {
            if (entity.value("emoji_id").toVariant().toLongLong() <= 0) return std::nullopt;
        } else if (type != u"bold"_q && type != u"italic"_q && type != u"underline"_q && type != u"strikethrough"_q && type != u"spoiler"_q) return std::nullopt;
    }
    return folder;
}
bool Parse(not_null<Main::Session*> session, const QJsonObject &object) {
    const auto revision = object.value("revision").toVariant().toLongLong();
    const auto limits = object.value("limits").toObject();
    if (!object.value("revision").isDouble() || revision < 0 || !limits.value("enabled").toBool()
        || limits.value("limit").toInt() != 30 || limits.value("chats_limit").toInt() != 200
        || limits.value("excluded_chats_limit").toInt() != 200 || limits.value("sharing_enabled").toBool()) return false;
    auto parsed = base::flat_map<int, Folder>();
    for (const auto &item : object.value("folders").toArray()) {
        auto folder = ParseFolder(item.toObject());
        if (!folder || parsed.contains(folder->id)) return false;
        parsed.emplace(folder->id, std::move(*folder));
    }
    auto order = std::vector<int>();
    for (const auto &item : object.value("order").toArray()) {
        const auto id = item.toInt(-1);
        if ((id != 0 && !parsed.contains(id)) || ranges::contains(order, id)) return false;
        order.push_back(id);
    }
    if (parsed.size() > 30 || order.size() != parsed.size() + 1 || !ranges::contains(order, 0)) return false;
    auto &state = Get(session);
    if (revision >= state.revision) { state.revision = revision; state.tags = object.value("tags_enabled").toBool(); state.folders = std::move(parsed); state.order = std::move(order); }
    return true;
}
MTPVector<MTPInputPeer> Peers(not_null<Main::Session*> session, const std::vector<qint64> &ids) {
    auto result = QVector<MTPInputPeer>();
    const auto bridge = BridgeFor(session);
    for (const auto id : ids) if (const auto history = bridge ? bridge->historyForChat(id) : nullptr) result.push_back(history->peer->input());
    return MTP_vector<MTPInputPeer>(result);
}
MTPDialogFilter Native(not_null<Main::Session*> session, const Folder &folder) {
    return MTP_dialogFilter(MTP_flags(folder.flags), MTP_int(folder.id),
        MTP_textWithEntities(MTP_string(folder.title), MTP_vector<MTPMessageEntity>(folder.entities)),
        MTP_string(folder.emoticon), MTP_int(folder.color.value_or(0)),
        Peers(session, folder.pinned), Peers(session, folder.include), Peers(session, folder.exclude));
}
QVector<MTPDialogFilter> Filters(not_null<Main::Session*> session) {
    auto result = QVector<MTPDialogFilter>(); const auto &state = Get(session);
    for (const auto id : state.order) result.push_back(id ? Native(session, state.folders.find(id)->second) : MTP_dialogFilterDefault());
    return result;
}
void Apply(not_null<Main::Session*> session) { session->data().chatsFilters().setPreloaded(Filters(session), Get(session).tags); }
void Next(not_null<Main::Session*> session) {
    auto &state = Get(session); if (state.loading || state.queue.empty()) return; state.loading = true;
    const auto weak = base::make_weak(session);
    state.queue.front()([weak] { if (!weak) return; auto &state = Get(weak.get()); state.queue.pop_front(); state.loading = false; Next(weak.get()); });
}
void Enqueue(not_null<Main::Session*> session, Fn<void(Fn<void()>)> action) { Get(session).queue.push_back(std::move(action)); Next(session); }
void Load(not_null<Main::Session*> session, Fn<void(bool)> done) {
    const auto weak = base::make_weak(session);
    ClientFor(session).communityRequest("GET", u"/me/folders"_q, {}, [weak, done](QJsonDocument doc, QString error, int status) {
        if (!weak) return;
        if (!error.isEmpty() || status != 200 || !Parse(weak.get(), doc.object())) { done(false); return; }
        const auto bridge = BridgeFor(weak.get()); if (!bridge) { done(false); return; }
        bridge->hydrateFolderChats([weak, done](QJsonDocument chats, bool ok) {
            if (!weak) return;
            if (ok) {
                auto &state = Get(weak.get()); state.chats.clear(); state.chatData.clear();
                for (const auto &value : chats.array()) {
                    const auto id = value.toObject().value("id").toVariant().toLongLong();
                    if (const auto history = BridgeFor(weak.get())->historyForChat(id)) { state.chats.emplace(history->peer->id, id); state.chatData.emplace(id, value.toObject()); }
                }
            }
            done(ok);
        });
    });
}
PeerId InputPeerId(const MTPInputPeer &peer, not_null<Main::Session*> session) {
    return peer.match([&](const MTPDinputPeerUser &data) { return peerFromUser(data.vuser_id().v); },
        [&](const MTPDinputPeerChannel &data) { return peerFromChannel(data.vchannel_id().v); },
        [&](const MTPDinputPeerChat &data) { return peerFromChat(data.vchat_id().v); },
        [&](const MTPDinputPeerSelf &) { return session->userPeerId(); }, [](const auto &) { return PeerId(); });
}
std::optional<QJsonArray> ChatIds(not_null<Main::Session*> session, const MTPVector<MTPInputPeer> &peers) {
    auto result = QJsonArray(); const auto &chats = Get(session).chats;
    for (const auto &peer : peers.v) {
        const auto found = chats.find(InputPeerId(peer, session)); if (found == chats.end()) return std::nullopt;
        result.push_back(QJsonValue(found->second));
    }
    return result;
}
std::optional<QJsonObject> Dto(not_null<Main::Session*> session, const MTPDialogFilter &filter) {
    if (filter.type() != mtpc_dialogFilter) return std::nullopt;
    const auto &data = filter.c_dialogFilter();
    auto include = ChatIds(session, data.vinclude_peers()), pinned = ChatIds(session, data.vpinned_peers()), exclude = ChatIds(session, data.vexclude_peers());
    if (!include || !pinned || !exclude) return std::nullopt;
    if (const auto previous = Get(session).folders.find(data.vid().v); previous != Get(session).folders.end()) {
        const auto retain = [&](QJsonArray &target, const std::vector<qint64> &ids) {
            for (const auto id : ids) if (!Get(session).chatData.contains(id) && !target.contains(QJsonValue(id))) target.push_back(QJsonValue(id));
        };
        retain(*include, previous->second.include); retain(*pinned, previous->second.pinned); retain(*exclude, previous->second.exclude);
    }
    auto result = QJsonObject{
        {"id", data.vid().v}, {"title", qs(data.vtitle().data().vtext())},
        {"title_entities", BridgeFor(session)->entitiesJson(data.vtitle().data().ventities(), qs(data.vtitle().data().vtext()))},
        {"emoticon", data.vemoticon() ? qs(*data.vemoticon()) : QString()}, {"static_title", data.is_title_noanimate()},
        {"contacts", data.is_contacts()}, {"non_contacts", data.is_non_contacts()}, {"groups", data.is_groups()},
        {"channels", data.is_broadcasts()}, {"bots", data.is_bots()}, {"exclude_muted", data.is_exclude_muted()},
        {"exclude_read", data.is_exclude_read()}, {"exclude_archived", data.is_exclude_archived()},
        {"include_chat_ids", *include}, {"pinned_chat_ids", *pinned}, {"exclude_chat_ids", *exclude} };
    if (data.vcolor()) result.insert("color", data.vcolor()->v);
    return result;
}
QByteArray EditKey(const MTPDialogFilter &filter, bool creating = false) {
    auto keyed = filter;
    if (creating && filter.type() == mtpc_dialogFilter) {
        const auto &data = filter.c_dialogFilter();
        keyed = MTP_dialogFilter(data.vflags(), MTP_int(0), data.vtitle(),
            data.vemoticon() ? *data.vemoticon() : MTP_string(""),
            data.vcolor() ? *data.vcolor() : MTP_int(0),
            data.vpinned_peers(), data.vinclude_peers(), data.vexclude_peers());
    }
    auto buffer = mtpBuffer();
    keyed.write(buffer);
    return QByteArray(reinterpret_cast<const char*>(buffer.constData()), int(buffer.size() * sizeof(mtpPrime)));
}
void Mutate(not_null<Main::Session*> session, QByteArray method, QString path, QJsonObject body, Mtp::Answer answer, Fn<void()> done, std::optional<qint64> revision = std::nullopt) {
    const auto weak = base::make_weak(session);
    body.insert("operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces)); body.insert("base_revision", QJsonValue(revision.value_or(Get(session).revision)));
    const auto finish = [weak, answer, done](QJsonDocument doc, QString error, int status) {
        if (!weak) return;
        if (error.isEmpty() && status == 200 && Parse(weak.get(), doc.object())) { answer.done(MTP_boolTrue()); Apply(weak.get()); done(); return; }
        if (status == 409 && doc.object().value("state").isObject()) Parse(weak.get(), doc.object().value("state").toObject());
        Apply(weak.get());
        answer.fail(Mtp::ErrorType(doc, error, status, u"FOLDER_NOT_FOUND"_q, u"PEER_ID_INVALID"_q), status >= 500 ? 500 : 400);
        Load(weak.get(), [weak, done](bool ok) { if (!weak) return; if (ok) Apply(weak.get()); done(); });
    };
    ClientFor(session).communityRequest(method, path, body, [weak, method, path, body, finish](QJsonDocument doc, QString error, int status) {
        if (!weak) return;
        if (!IsTransportFailure(status) && status < 500 && status != 408 && status != 429) { finish(doc, error, status); return; }
        ClientFor(weak.get()).operationResult(body.value("operation_id").toString(), [weak, method, path, body, finish](QJsonDocument journal, QString error, int status) {
            if (!weak) return;
            const auto object = journal.object();
            if (error.isEmpty() && object.value("status").toString() == u"done"_q) finish(QJsonDocument(object.value("result").toObject()), {}, 200);
            else if (error.isEmpty() && object.value("status").toString() == u"unknown"_q) ClientFor(weak.get()).communityRequest(method, path, body, finish);
            else finish(journal, error, status);
        });
    });
}
}
void BindEditor(not_null<Main::Session*> session,
        const Data::ChatFilter &initial,
        Fn<void(const Data::ChatFilter &)> &done,
        Fn<void(const Data::ChatFilter &, Fn<void(Data::ChatFilter)>)> &saveAnd) {
    const auto weak = base::make_weak(session);
    auto &state = Get(session);
    const auto key = EditKey(initial.tl());
    auto revision = state.revision;
    if (const auto draft = state.editRevisions.find(key); draft != state.editRevisions.end()) {
        revision = draft->second;
    } else if (const auto confirmed = state.folders.find(initial.id()); confirmed != state.folders.end()) {
        const auto current = Data::ChatFilter::FromTL(Native(session, confirmed->second), &session->data());
        if (key != EditKey(current.tl())) revision = -1;
    }
    const auto remember = [weak, revision](const Data::ChatFilter &filter) {
        Get(weak.get()).editRevisions[EditKey(filter.tl())] = revision;
    };
    done = [weak, remember, callback = done](const Data::ChatFilter &filter) {
        if (!weak) return;
        remember(filter);
        callback(filter);
    };
    if (saveAnd) {
        saveAnd = [weak, remember, callback = saveAnd](const Data::ChatFilter &filter, Fn<void(Data::ChatFilter)> next) {
            if (!weak) return;
            remember(filter);
            callback(filter, std::move(next));
        };
    }
}
bool Supported(Main::Session *session) { return session && CurrentUser(session).value("capabilities").toArray().contains("chat-folders-v1") && CurrentUser(session).value("folders").toObject().value("enabled").toBool(); }
bool SharingEnabled(Main::Session *session) { return !Enabled() || kSharingEnabled; }
void Refresh(not_null<Main::Session*> session, bool reset) {
    if (!Supported(session)) return;
    if (reset) Get(session).revision = -1;
    const auto weak = base::make_weak(session);
    Enqueue(session, [weak](Fn<void()> done) { if (!weak) return; Load(weak.get(), [weak, done](bool ok) { if (!weak) return; if (ok) Apply(weak.get()); done(); }); });
}
void Receive(not_null<Main::Session*> session, const QJsonObject &state) {
    if (!Supported(session) || !Parse(session, state)) return;
    Refresh(session);
}
bool Intercepts(const MTP::details::SerializedRequest &request) {
    if (IsSharingRequest(Mtp::RequestType(request))) return true;
    switch (Mtp::RequestType(request)) {
    case mtpc_messages_getPeerDialogs: case mtpc_messages_getDialogFilters: case mtpc_messages_getSuggestedDialogFilters: case mtpc_messages_updateDialogFilter:
    case mtpc_messages_updateDialogFiltersOrder: case mtpc_messages_toggleDialogFilterTags: case mtpc_help_getAppConfig: return true;
    default: return false;
    }
}
void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId, const MTP::details::SerializedRequest &request) {
    const auto answer = Mtp::Answer(instance, requestId); const auto session = Mtp::SessionFor(instance);
    if (!session) { answer.fail(u"SESSION_NOT_FOUND"_q); return; }
    const auto type = Mtp::RequestType(request);
    if (IsSharingRequest(type) && !SharingEnabled(session)) { answer.fail(u"FOLDER_SHARING_DISABLED"_q); return; }
    auto reader = Mtp::Reader(request);
    if (type == mtpc_help_getAppConfig) {
        auto entries = QVector<MTPJSONObjectValue>();
        const auto put = [&](QString key, MTPJSONValue value) { entries.push_back(MTP_jsonObjectValue(MTP_string(key), value)); };
        put(u"dialog_filters_enabled"_q, MTP_jsonBool(MTP_bool(Supported(session))));
        for (const auto &suffix : { u"default"_q, u"premium"_q }) {
            put(u"dialog_filters_limit_"_q + suffix, MTP_jsonNumber(MTP_double(30)));
            put(u"dialog_filters_chats_limit_"_q + suffix, MTP_jsonNumber(MTP_double(200)));
            put(u"dialog_filters_pinned_limit_"_q + suffix, MTP_jsonNumber(MTP_double(200)));
        }
        answer.done(MTP_help_appConfig(MTP_int(0x464F4C44), MTP_jsonObject(MTP_vector<MTPJSONObjectValue>(entries)))); return;
    }
    if (!Supported(session)) { answer.fail(u"FOLDERS_DISABLED"_q); return; }
    if (type == mtpc_messages_getPeerDialogs) {
        const auto requested = reader.read<MTPVector<MTPInputDialogPeer>>();
        if (!reader.ok()) { answer.fail(u"INPUT_CONSTRUCTOR_INVALID"_q); return; }
        const auto weak = base::make_weak(session);
        Enqueue(session, [weak, requested, answer](Fn<void()> done) {
            if (!weak) return;
            Load(weak.get(), [weak, requested, answer, done](bool ok) {
                if (!weak) return;
                if (!ok) { answer.fail(u"FOLDER_LOAD_FAILED"_q); done(); return; }
                auto dialogs = QVector<MTPDialog>();
                const auto &state = Get(weak.get());
                for (const auto &input : requested.v) {
                    if (input.type() != mtpc_inputDialogPeer) continue;
                    const auto peerId = InputPeerId(input.c_inputDialogPeer().vpeer(), weak.get());
                    const auto found = state.chats.find(peerId);
                    if (found == state.chats.end()) continue;
                    const auto dto = state.chatData.find(found->second)->second;
                    const auto history = BridgeFor(weak.get())->historyForChat(found->second);
                    const auto receipts = dto.value("receipt_state").toObject();
                    auto flags = MTPDdialog::Flags();
                    if (dto.value("archived").toBool()) flags |= MTPDdialog::Flag::f_folder_id;
                    if (dto.value("pinned").toBool()) flags |= MTPDdialog::Flag::f_pinned;
                    if (dto.value("marked_unread").toBool()) flags |= MTPDdialog::Flag::f_unread_mark;
                    dialogs.push_back(MTP_dialog(MTP_flags(flags), peerToMTP(history->peer->id),
                        MTP_int(dto.value("last_message").toObject().value("id").toInt()),
                        MTP_int(receipts.value("inbox").toObject().value("read_through_id").toInt()),
                        MTP_int(receipts.value("outbox").toObject().value("read_through_id").toInt()),
                        MTP_int(dto.value("unread_count").toInt()), MTP_int(0), MTP_int(0), MTP_int(0),
                        BridgeFor(weak.get())->communityNotifySettings(found->second, dto), MTP_int(0), MTPDraftMessage(), MTP_int(1), MTP_int(0)));
                }
                answer.done(MTP_messages_peerDialogs(MTP_vector<MTPDialog>(dialogs), MTP_vector<MTPMessage>(), MTP_vector<MTPChat>(), MTP_vector<MTPUser>(), MTP_updates_state(MTP_int(0), MTP_int(0), MTP_int(base::unixtime::now()), MTP_int(0), MTP_int(0))));
                done();
            });
        });
        return;
    }

    auto body = QJsonObject(); auto path = QString(); auto method = QByteArray("PUT");
    if (type == mtpc_messages_updateDialogFilter) {
        const auto flags = reader.flags(); const auto id = reader.read<MTPint>().v; path = u"/me/folders/"_q + QString::number(id);
        if (flags & 1) {
            const auto filter = reader.read<MTPDialogFilter>();
            auto revision = std::optional<qint64>();
            auto &state = Get(session);
            auto key = EditKey(filter);
            if (!state.editRevisions.contains(key)) key = EditKey(filter, true);
            if (const auto found = state.editRevisions.find(key); found != state.editRevisions.end()) {
                revision = found->second; state.editRevisions.remove(key);
            } else if (state.revision >= 0) { revision = state.revision; }
            const auto weak = base::make_weak(session);
            Enqueue(session, [weak, filter, id, answer, path, revision](Fn<void()> done) {
                if (!weak) return;
                const auto send = [weak, filter, id, answer, path, done, revision](bool ok) {
                    if (!weak) return; auto dto = ok ? Dto(weak.get(), filter) : std::nullopt;
                    if (!dto || (revision && *revision < 0)) {
                        if (Get(weak.get()).revision >= 0) Apply(weak.get());
                        answer.fail(u"FOLDER_INVALID"_q); done(); return;
                    }
                    dto->insert("id", id);
                    Mutate(weak.get(), "PUT", path, QJsonObject{{"folder", *dto}}, answer, done, revision);
                };
                Load(weak.get(), send);
            }); return;
        } else { method = "DELETE"; }
    } else if (type == mtpc_messages_updateDialogFiltersOrder) {
        auto order = QJsonArray(); for (const auto &id : reader.read<MTPVector<MTPint>>().v) order.push_back(id.v);
        if (!order.contains(0)) order.prepend(0); body.insert("order", order); path = u"/me/folders/order"_q;
    } else if (type == mtpc_messages_toggleDialogFilterTags) {
        body.insert("tags_enabled", reader.read<MTPBool>().type() == mtpc_boolTrue); path = u"/me/folders/settings"_q;
    }
    if (!reader.ok()) { answer.fail(u"INPUT_CONSTRUCTOR_INVALID"_q); return; }
    const auto weak = base::make_weak(session);
    Enqueue(session, [weak, type, answer, body, path, method](Fn<void()> done) {
        if (!weak) return;
        if (type == mtpc_messages_getDialogFilters) {
            Load(weak.get(), [weak, answer, done](bool ok) {
                if (!weak) return;
                if (ok) answer.done(MTP_messages_dialogFilters(MTP_flags(Get(weak.get()).tags ? MTPDmessages_dialogFilters::Flag::f_tags_enabled : MTPDmessages_dialogFilters::Flag(0)), MTP_vector<MTPDialogFilter>(Filters(weak.get()))));
                else answer.fail(u"FOLDER_LOAD_FAILED"_q); done();
            });
        } else if (type == mtpc_messages_getSuggestedDialogFilters) {
            ClientFor(weak.get()).communityRequest("GET", u"/me/folders/suggested"_q, {}, [weak, answer, done](QJsonDocument doc, QString error, int status) {
                if (!weak) return;
                if (!error.isEmpty() || status != 200) answer.fail(u"FOLDER_LOAD_FAILED"_q);
                else {
                    auto suggestions = QVector<MTPDialogFilterSuggested>();
                    for (const auto &value : doc.object().value("folders").toArray()) {
                        const auto dto = value.toObject(); const auto folder = dto.value("folder").toObject();
                        if (const auto parsed = ParseFolder(folder)) suggestions.push_back(MTP_dialogFilterSuggested(Native(weak.get(), *parsed), MTP_string(dto.value("description").toString())));
                    }
                    answer.done(MTP_vector<MTPDialogFilterSuggested>(suggestions));
                }
                done();
            });
        } else {
            const auto send = [weak, method, path, body, answer, done](bool ok) { if (!weak) return; if (!ok) { if (Get(weak.get()).revision >= 0) Apply(weak.get()); answer.fail(u"FOLDER_LOAD_FAILED"_q); done(); return; } Mutate(weak.get(), method, path, body, answer, done); };
            if (Get(weak.get()).revision < 0) Load(weak.get(), send); else send(true);
        }
    });
}
}
