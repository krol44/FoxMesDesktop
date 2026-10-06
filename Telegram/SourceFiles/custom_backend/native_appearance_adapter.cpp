#include "custom_backend/native_appearance_adapter.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "base/flat_map.h"
#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "ui/chat/chat_style.h"
#include "ui/effects/outline_segments.h"
#include "ui/painter.h"
#include "api/api_peer_colors.h"
#include "apiwrap.h"
#include "styles/style_foxmes_appearance.h"
#include <QtCore/QUuid>
#include <deque>

namespace CustomBackend::Appearance {
namespace {
constexpr uint64 kSetId = 520093696;
struct PeerAppearance {
    std::optional<int> name, profile;
    DocumentId background = 0, profileBackground = 0;
    qint64 revision = 0;
    static PeerAppearance Parse(const QJsonObject &value) {
        auto result = PeerAppearance();
        if (value.value("name_color_id").isDouble()) result.name = value.value("name_color_id").toInt();
        if (value.value("profile_color_id").isDouble()) result.profile = value.value("profile_color_id").toInt();
        result.background = value.value("background_emoji_id").toVariant().toULongLong();
        result.profileBackground = value.value("profile_background_emoji_id").toVariant().toULongLong();
        result.revision = value.value("revision").toVariant().toLongLong();
        return result;
    }
};
struct Icon { DocumentId id; QString emoji, url; qint64 size; };
struct Mutation { QJsonObject body; Mtp::Answer answer; };
struct State {
    base::flat_map<qint64, PeerAppearance> appearances;
    base::flat_map<DocumentId, std::shared_ptr<Data::DocumentMedia>> media;
    std::deque<Mutation> queue;
    bool loading = false;
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
QVector<MTPint> Colors(const QJsonArray &array) {
    auto result = QVector<MTPint>();
    for (const auto &value : array) result.push_back(MTP_int(value.toInt()));
    return result;
}
struct ColorOption {
    int id = 0;
    QVector<MTPint> light, dark, palette, darkPalette;
};
struct ColorCatalog {
    int hash = 0;
    std::vector<ColorOption> options;
    static ColorCatalog Parse(const QJsonObject &value) {
        auto result = ColorCatalog();
        result.hash = value.value("hash").toInt();
        for (const auto &item : value.value("colors").toArray()) {
            const auto object = item.toObject();
            auto option = ColorOption{
                object.value("id").toInt(),
                Colors(object.value("light").toArray()), Colors(object.value("dark").toArray()),
                Colors(object.value("palette").toArray()), Colors(object.value("dark_palette").toArray()) };
            if (option.id >= 0 && option.id <= 20 && !option.light.empty() && option.light.size() <= 3 && !option.dark.empty() && option.dark.size() <= 3) {
                result.options.push_back(std::move(option));
            }
        }
        return result;
    }
};
MTPhelp_PeerColors Palette(const ColorCatalog &catalog, bool profile) {
    auto options = QVector<MTPhelp_PeerColorOption>();
    for (const auto &option : catalog.options) {
        const auto pack = [&](bool isDark) -> MTPhelp_PeerColorSet {
            if (!profile) return MTP_help_peerColorSet(MTP_vector<MTPint>(isDark ? option.dark : option.light));
            return MTP_help_peerColorProfileSet(
                MTP_vector<MTPint>(isDark ? option.darkPalette : option.palette),
                MTP_vector<MTPint>(isDark ? option.dark : option.light), MTP_vector<MTPint>(isDark ? option.dark : option.light));
        };
        using F = MTPDhelp_peerColorOption::Flag;
        options.push_back(MTP_help_peerColorOption(MTP_flags(F::f_colors | F::f_dark_colors),
            MTP_int(option.id), pack(false), pack(true), MTPint(), MTPint()));
    }
    return MTP_help_peerColors(MTP_int(catalog.hash), MTP_vector<MTPhelp_PeerColorOption>(options));
}
void Run(not_null<Main::Session*> session) {
    auto &state = Get(session);
    if (state.queue.empty()) return;
    const auto body = state.queue.front().body;
    const auto weak = base::make_weak(session);
    const auto finish = [weak](QJsonDocument doc, QString error, int status) {
        const auto session = weak.get();
        if (!session) return;
        auto &state = Get(session);
        if (state.queue.empty()) return;
        const auto answer = state.queue.front().answer;
        if (error.isEmpty() && status >= 200 && status < 300 && doc.object().contains("peer_appearance")) {
            Apply(session, doc.object());
            RememberUser(session, doc.object());
            answer.done(MTP_boolTrue());
        } else {
            answer.fail(u"APPEARANCE_SAVE_FAILED"_q, 500);
            ClientFor(session).me([weak](QJsonDocument doc, QString error, int status) {
                if (weak && error.isEmpty() && status == 200) { Apply(weak.get(), doc.object()); RememberUser(weak.get(), doc.object()); }
            });
        }
        state.queue.pop_front();
        Run(session);
    };
    ClientFor(session).communityRequest("PATCH", u"/me/appearance"_q, body,
        [weak, body, finish](QJsonDocument doc, QString error, int status) {
            if (!weak) return;
            if (!IsTransportFailure(status) && status < 500 && status != 408) { finish(doc, error, status); return; }
            const auto operation = body.value("operation_id").toString();
            ClientFor(weak.get()).operationResult(operation,
                [weak, body, finish](QJsonDocument journal, QString error, int status) {
                    if (!weak) return;
                    const auto object = journal.object();
                    if (error.isEmpty() && object.value("status").toString() == "done") {
                        finish(QJsonDocument(object.value("result").toObject()), {}, 200);
                    } else if (error.isEmpty() && object.value("status").toString() == "unknown") {
                        ClientFor(weak.get()).communityRequest("PATCH", u"/me/appearance"_q, body, finish);
                    } else { finish(journal, error, status); }
                });
        });
}
void Catalog(not_null<Main::Session*> session, Fn<void(QVector<MTPlong>, QVector<MTPDocument>)> done) {
    const auto weak = base::make_weak(session);
    ClientFor(session).communityRequest("GET", u"/background-emojis"_q, {},
        [weak, done](QJsonDocument doc, QString error, int status) {
            if (!weak) return;
            auto ids = QVector<MTPlong>();
            auto documents = QVector<MTPDocument>();
            if (error.isEmpty() && status == 200) {
                auto &stickers = weak.get()->data().stickers();
                if (stickers.emojiSetsOrderRef().removeAll(kSetId)) {
                    stickers.setsRef().remove(kSetId);
                    stickers.notifyUpdated(Data::StickersType::Emoji);
                }
                for (const auto &item : doc.object().value("emojis").toArray()) {
                    const auto value = item.toObject();
                    const auto icon = Icon{ value.value("id").toVariant().toULongLong(),
                        value.value("emoji").toString(), value.value("asset_url").toString(), value.value("size").toVariant().toLongLong() };
                    if (!icon.id || !icon.url.startsWith(u"/background-emojis/"_q)) continue;
                    using F = MTPDdocumentAttributeCustomEmoji::Flag;
                    const auto attributes = QVector<MTPDocumentAttribute>{
                        MTP_documentAttributeFilename(MTP_string(u"background.webp"_q)),
                        MTP_documentAttributeImageSize(MTP_int(128), MTP_int(128)),
                        MTP_documentAttributeCustomEmoji(MTP_flags(F::f_free | F::f_text_color),
                            MTP_string(icon.emoji), MTP_inputStickerSetID(MTP_long(kSetId), MTP_long(0))) };
                    ids.push_back(MTP_long(icon.id));
                    documents.push_back(MTP_document(MTP_flags(0), MTP_long(icon.id), MTP_long(0), MTP_bytes(QByteArray()),
                        MTP_int(base::unixtime::now()), MTP_string(u"image/webp"_q), MTP_long(icon.size),
                        MTPVector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(0), MTP_vector<MTPDocumentAttribute>(attributes)));
                    ClientFor(weak.get()).downloadFile(icon.url, [weak, icon, attributes](QByteArray bytes, QString error, int status) {
                        if (!weak || !error.isEmpty() || status != 200) return;
                        if (bytes.size() < 12 || bytes.left(4) != "RIFF" || bytes.mid(8, 4) != "WEBP") return;
                        const auto session = weak.get();
                        const auto document = session->data().document(icon.id, 0, QByteArray(), base::unixtime::now(), attributes,
                            u"image/webp"_q, InlineImageLocation(), ImageWithLocation(), ImageWithLocation(), false, 0, bytes.size());
                        auto media = document->createMediaView();
                        media->setBytes(bytes);
                        Get(session).media[icon.id] = media;
                        session->data().customEmojiManager().resolveLocalDocument(document);
                        session->notifyDownloaderTaskFinished();
                        session->data().stickers().notifyUpdated(Data::StickersType::Emoji);
                    });
                }
            }
            done(std::move(ids), std::move(documents));
        });
}
} // namespace
void Load(not_null<Main::Session*> session) {
    auto &state = Get(session);
    if (state.loading) return;
    state.loading = true;
    const auto weak = base::make_weak(session);
    Catalog(session, [weak](auto, auto documents) { if (weak && documents.empty()) Get(weak.get()).loading = false; });
}
void Apply(not_null<Main::Session*> session, const QJsonObject &user) {
    if (!user.value("peer_appearance").isObject()) return;
    const auto id = user.value("id").toVariant().toLongLong();
    if (id <= 0) return;
    const auto value = PeerAppearance::Parse(user.value("peer_appearance").toObject());
    if (value.background || value.profileBackground) Load(session);
    auto &appearances = Get(session).appearances;
    const auto i = appearances.find(id);
    if (i != appearances.end() && i->second.revision > value.revision) return;
    appearances[id] = value;
    const auto peer = session->data().user(UserId(id));
    if (value.name) peer->changeColorIndex(*value.name); else peer->clearColorIndex();
    if (value.profile) peer->changeColorProfileIndex(*value.profile); else peer->clearColorProfileIndex();
    peer->changeBackgroundEmojiId(value.background);
    peer->changeProfileBackgroundEmojiId(value.profileBackground);
    using F = Data::PeerUpdate::Flag;
    session->changes().peerUpdated(peer, F::Color | F::ColorProfile | F::BackgroundEmoji);
}
void PaintReply(PeerData *peer, const Ui::ChatPaintContext &context,
        bool inBubble, Fn<void(const Ui::ChatPaintContext &)> paint) {
    auto styled = context;
    if (inBubble && peer && peer->isUser()) {
        const auto &appearances = Get(&peer->session()).appearances;
        const auto i = appearances.find(peerToUser(peer->id).bare);
        if (i != appearances.end() && (i->second.name || i->second.background)) {
            styled.outbg = false;
        }
    }
    paint(styled);
}
void PaintProfileOutline(not_null<PeerData*> peer, QPainter &p,
        const QRect &geometry, float64 progress, bool nativeOutline, Fn<void()> paint) {
    paint();
    if (nativeOutline || !peer->isUser() || !peer->colorProfileIndex()) return;
    const auto colors = peer->session().api().peerColors().colorProfileFor(peer);
    if (!colors) return;
    constexpr auto kFadeEnd = .4;
    constexpr auto kFadeRange = .6;
    const auto alpha = std::clamp((progress - kFadeEnd) / kFadeRange, 0., 1.);
    if (alpha <= 0.) return;
    const auto inset = st::foxmesProfileOutlineInset;
    const auto rect = QRectF(geometry).adjusted(-inset, -inset, inset, inset);
    const auto brush = colors->story.size() > 1
        ? Ui::UnreadStoryOutlineGradient(rect, colors->story[0], colors->story[1])
        : Ui::UnreadStoryOutlineGradient(rect);
    p.save();
    auto hq = PainterHighQualityEnabler(p);
    p.setOpacity(alpha);
    Ui::PaintOutlineSegments(p, rect, { {
        .brush = brush,
        .width = float64(st::foxmesProfileOutlineWidth),
    } });
    p.restore();
}
bool Intercepts(const MTP::details::SerializedRequest &request) {
    const auto type = Mtp::RequestType(request);
    if (type == mtpc_help_getPeerColors || type == mtpc_help_getPeerProfileColors || type == mtpc_account_updateColor
        || type == mtpc_account_getDefaultBackgroundEmojis) return true;
    if (type == mtpc_messages_getStickerSet) {
        auto reader = Mtp::Reader(request);
        return reader.read<MTPInputStickerSet>().type() == mtpc_inputStickerSetEmojiDefaultStatuses;
    }
    return false;
}
void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId, const MTP::details::SerializedRequest &request) {
    const auto answer = Mtp::Answer(instance, requestId);
    const auto type = Mtp::RequestType(request);
    auto body = QJsonObject();
    if (type == mtpc_account_updateColor) {
        auto reader = Mtp::Reader(request);
        const auto flags = reader.flags();
        auto choice = QJsonObject();
        if (flags & 4) {
            const auto color = reader.read<MTPPeerColor>();
            if (color.type() != mtpc_peerColor) { answer.fail(u"COLOR_INVALID"_q); return; }
            const auto &data = color.c_peerColor();
            if (data.vcolor()) choice.insert("color_id", data.vcolor()->v);
            if (data.vbackground_emoji_id()) choice.insert("background_emoji_id", qint64(data.vbackground_emoji_id()->v));
        }
        if (!reader.ok()) { answer.fail(u"COLOR_INVALID"_q); return; }
        body.insert((flags & 2) ? "profile" : "name", choice);
        body.insert("operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    crl::on_main([instance = QPointer<MTP::Instance>(instance.get()), type, body, answer] {
        if (!instance) return;
        const auto session = Mtp::SessionFor(instance.data());
        if (!session) { answer.fail(u"SESSION_REVOKED"_q, 401); return; }
        if (type == mtpc_account_updateColor) {
            auto &queue = Get(session).queue;
            queue.push_back({body, answer});
            if (queue.size() == 1) Run(session);
        } else if (type == mtpc_help_getPeerColors || type == mtpc_help_getPeerProfileColors) {
            const auto profile = type == mtpc_help_getPeerProfileColors;
            ClientFor(session).communityRequest("GET", profile ? u"/peer-colors?scope=profile"_q : u"/peer-colors?scope=replies"_q, {},
                [answer, profile](QJsonDocument doc, QString error, int status) {
                    if (!error.isEmpty() || status != 200) answer.fail(u"COLOR_CATALOG_UNAVAILABLE"_q, 500);
                    else answer.done(Palette(ColorCatalog::Parse(doc.object()), profile));
                });
        } else {
            Catalog(session, [answer, type](QVector<MTPlong> ids, QVector<MTPDocument> documents) {
                if (type == mtpc_account_getDefaultBackgroundEmojis) {
                    answer.done(MTP_emojiList(MTP_long(1), MTP_vector<MTPlong>(ids)));
                } else {
                    using F = MTPDstickerSet::Flag;
                    const auto set = MTP_stickerSet(MTP_flags(F::f_emojis | F::f_text_color), MTPint(), MTP_long(kSetId), MTP_long(0),
                        MTP_string(u"Background icons"_q), MTPstring(), MTPVector<MTPPhotoSize>(), MTPint(), MTPint(), MTPlong(), MTP_int(documents.size()), MTP_int(1));
                    answer.done(MTP_messages_stickerSet(set, MTPVector<MTPStickerPack>(), MTPVector<MTPStickerKeyword>(), MTP_vector<MTPDocument>(documents)));
                }
            });
        }
    });
}
} // namespace CustomBackend::Appearance
