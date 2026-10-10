/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_chat_themes_adapter.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_wallpaper_adapter.h"
#include <QtCore/QJsonArray>
#include "data/data_cloud_themes.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_wall_paper.h"
#include "history/history.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "window/themes/window_theme.h"
#include "styles/style_layers.h"

#include <QtCore/QRegularExpression>

namespace CustomBackend::ChatThemes {
namespace {

[[nodiscard]] std::vector<Data::CloudTheme> BuildCatalog(
        not_null<Main::Session*> session, const QJsonObject &catalog) {
    auto result = std::vector<Data::CloudTheme>();
    for (const auto &value : catalog.value("items").toArray()) {
        const auto object = value.toObject();
        auto theme = Data::CloudTheme();
        const auto personal = object.value("emoticon").toString().isEmpty();
        const auto idBase = personal ? quint64(1000000000) : quint64(900000000);
        theme.id = idBase + object.value("id").toVariant().toULongLong();
        if (personal) theme.createdBy = session->userId();
        const auto palette = object.value("desktop_palette").toObject();
        if (personal && !palette.isEmpty()) {
            auto bytes = QByteArray();
            for (auto i = palette.begin(); i != palette.end(); ++i) bytes += i.key().toUtf8() + ": " + i.value().toString().toUtf8() + ";\n";
            theme.documentId = DocumentId(1400000000 + object.value("id").toVariant().toULongLong());
            const auto document = session->data().processDocument(MTP_document(
                MTP_flags(0), MTP_long(theme.documentId), MTP_long(0), MTP_bytes(), MTP_int(base::unixtime::now()),
                MTP_string(u"application/x-tgtheme-tdesktop"_q), MTP_long(bytes.size()), MTPVector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(0),
                MTP_vector<MTPDocumentAttribute>(QVector<MTPDocumentAttribute>{MTP_documentAttributeFilename(MTP_string(u"theme.tdesktop-palette"_q))})));
            document->setDataAndCache(bytes);
        }

        theme.emoticon = object.value("emoticon").toString();
        theme.title = object.value("title").toString();
        theme.slug = u"foxmes-"_q + QString::number(theme.id - idBase);
        for (const auto &entry : object.value("settings").toArray()) {
            const auto settings = entry.toObject();
            const auto base = settings.value("base_theme").toString();
            auto native = Data::CloudTheme::Settings();
            native.accentColor = QColor::fromRgb(settings.value("accent_color").toInt());
            if (settings.value("outgoing_accent_color").isDouble()) native.outgoingAccentColor = QColor::fromRgb(settings.value("outgoing_accent_color").toInt());
            for (const auto &color : settings.value("message_colors").toArray()) native.outgoingMessagesColors.push_back(QColor::fromRgb(color.toInt()));
            if (settings.value("wallpaper").isObject()) {
                const auto paper = settings.value("wallpaper").toObject();
                native.paper = Data::WallPaper::Create(session, Wallpapers::NativePaper(session, QJsonObject{
                    { "wallpaper", paper }, { "intensity", paper.value("intensity") },
                }));
            }
            const auto type = (base == u"night"_q || base == u"tinted"_q)
                ? Data::CloudThemeType::Dark
                : Data::CloudThemeType::Light;
            if (personal && !palette.isEmpty()
                && (base == u"classic"_q || base == u"tinted"_q)) {
                theme.settings[type] = std::move(native);
            } else {
                theme.settings.emplace(type, std::move(native));
            }
        }
        result.push_back(std::move(theme));
    }
    return result;
}

} // namespace

void Apply(
		not_null<Data::CloudThemes*> themes,
		std::vector<Data::CloudTheme> list) {
	themes->_chatThemes = std::move(list);
	themes->_myGiftThemesLoaded = true;
	crl::on_main(themes->_session, [themes] {
		themes->_chatThemesUpdates.fire({});
		themes->_myGiftThemesUpdates.fire({});
	});
}

void Request(not_null<Main::Session*> session) {
    const auto weak = base::make_weak(session);
    ClientFor(session).chatThemes([weak](QJsonDocument doc, QString error, int) {
        const auto strong = weak.get();
        if (!strong || !BridgeFor(strong) || !error.isEmpty() || !doc.isObject()) return;
        Apply(&strong->data().cloudThemes(), BuildCatalog(strong, doc.object()));
    });
}

void ApplyUserThemes(not_null<Data::CloudThemes*> themes, std::vector<Data::CloudTheme> list) {
    themes->_list = std::move(list);
    themes->_updates.fire({});
}
void RequestUserThemes(not_null<Main::Session*> session) {
    const auto weak = base::make_weak(session);
    ClientFor(session).userThemes([weak](QJsonDocument doc, QString error, int) {
        const auto strong = weak.get();
        if (strong && BridgeFor(strong) && error.isEmpty() && doc.isObject()) ApplyUserThemes(&strong->data().cloudThemes(), BuildCatalog(strong, doc.object()));
    });
}
void RemoveUserTheme(not_null<Main::Session*> session, quint64 id) {
    if (id < 1000000000) return;
    const auto weak = base::make_weak(session);
    ClientFor(session).deleteUserTheme(id - 1000000000, [weak](QJsonDocument, QString error, int) {
        if (const auto strong = weak.get(); strong && error.isEmpty()) RequestUserThemes(strong);
    });
}
Fn<void()> SaveUserTheme(not_null<Main::Session*> session, const Data::CloudTheme &fields, const QByteArray &palette, const QImage &background, Fn<void(Data::CloudTheme)> done, Fn<void(QString)> fail) {
    auto values = QJsonObject();
    const auto expression = QRegularExpression(u"([A-Za-z][A-Za-z0-9_]*):\\s*([^;]+);"_q);
    auto matches = expression.globalMatch(QString::fromUtf8(palette));
    while (matches.hasNext()) { const auto match = matches.next(); values[match.captured(1)] = match.captured(2).trimmed(); }
    if (values.isEmpty()) { fail(u"THEME_FILE_INVALID"_q); return [] {}; }
    const auto active = std::make_shared<bool>(true);
    const auto weak = base::make_weak(session);
    const auto id = fields.createdBy == session->userId() && fields.id >= 1000000000 ? fields.id - 1000000000 : 0;
    // Keep the editor's values while its wallpaper upload is pending.
    const auto editorPaper = Window::Theme::Background()->paper();
    const auto baseTheme = Window::Theme::IsNightMode() ? "tinted" : "classic";
    const auto accent = int(st::windowActiveTextFg->c.rgb() & 0xffffff);
    const auto messageColor = int(st::msgOutBg->c.rgb() & 0xffffff);
    const auto send = [=](std::optional<Data::WallPaper> paper) {
        if (!*active || !weak.get()) return;
        auto settings = QJsonArray();
        const auto source = paper ? *paper : editorPaper;
        if (!id || paper || (Data::IsCloudWallPaper(source) && !source.isLocal())) {
            const auto wallpaper = Wallpapers::StateFromPaper(session, source).value("wallpaper");
            settings.push_back(QJsonObject{
                { "base_theme", baseTheme },
                { "accent_color", accent },
                { "outgoing_accent_color", accent },
                { "message_colors", QJsonArray{messageColor} },
                { "animate_message_colors", false }, { "wallpaper", wallpaper.isUndefined() ? QJsonValue(QJsonValue::Null) : wallpaper },
            });
        }
        ClientFor(session).saveUserTheme(id, QJsonObject{{ "title", fields.title }, { "settings", settings }, { "desktop_palette", values }},
[=](QJsonDocument doc, QString error, int) {
            const auto strong = weak.get();
            if (!*active || !strong || !BridgeFor(strong)) return;
            if (!error.isEmpty() || !doc.isObject()) { fail(error); return; }
            const auto list = BuildCatalog(strong, QJsonObject{{ "items", QJsonArray{doc.object()} }});
            if (list.empty()) { fail(u"THEME_FILE_INVALID"_q); return; }
            done(list.front());
        });
    };
    if (background.isNull()) send(std::nullopt);
    else Wallpapers::Upload(session, background, [=](std::optional<Data::WallPaper> paper) {
        if (!paper) { if (*active) fail(u"WALLPAPER_UPLOAD_FAILED"_q); return; }
        send(paper);
    });

    return [active] { *active = false; };
}

void Save(not_null<PeerData*> peer, const QString &emoticon) {
	const auto session = &peer->session();
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	const auto history = session->data().historyLoaded(peer->id);
	if (!history) {
		return;
	}
	const auto weak = base::make_weak(session);
	bridge->resolveChatId(history, [weak, emoticon](qint64 chatId) {
		const auto strong = weak.get();
		if (!strong || chatId <= 0 || !BridgeFor(strong)) {
			return;
		}
        ClientFor(strong).setChatAppearance(chatId, QJsonObject{
            { "theme_emoticon", emoticon }, { "reset_wallpaper", "self" },
        }, [weak, chatId](QJsonDocument doc, QString error, int) {
            const auto strong = weak.get();
            if (!strong || !BridgeFor(strong)) return;
            if (error.isEmpty() && doc.isObject()) {
                BridgeFor(strong)->applyAppearanceResult(chatId, doc.object());
            } else {
                BridgeFor(strong)->reloadChats();
                if (const auto controller = strong->tryResolveWindow()) controller->showToast(error);
            }
        });
	});
}

void ApplyForPeer(not_null<PeerData*> peer, const QString &emoticon) {
	if (peer->themeToken() != emoticon) {
		peer->setThemeToken(emoticon);
	}
}

}
