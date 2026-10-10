/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_wallpaper_adapter.h"

#include "base/unixtime.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_document.h"
#include "ui/chat/chat_theme.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_wall_paper.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "ui/boxes/confirm_box.h"
#include "lang/lang_keys.h"
#include "styles/style_layers.h"
#include "window/themes/window_theme.h"
#include "window/themes/window_theme_preview.h"
#include "window/themes/window_themes_embedded.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtGui/QImageWriter>

namespace CustomBackend::Wallpapers {
namespace {

constexpr auto kWallPaperIdOffset = quint64(4000000000000000ULL);

constexpr auto kUploadJpegQuality = 87;

constexpr auto kNoBackgroundId = quint64(4000000000000001ULL);

struct State {
    qint64 defaultRevision = 0;
    quint64 defaultGeneration = 0;
	base::flat_map<quint64, QString> sha256ById;
    base::flat_map<quint64, QJsonObject> choicesById;
};

base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	return States()[session];
}

[[nodiscard]] quint64 IdFor(const QString &sha256) {
	auto value = quint64(0);
	const auto count = std::min(qsizetype(15), sha256.size());
	for (auto i = qsizetype(0); i != count; ++i) {
		const auto symbol = sha256.at(i);
		const auto digit = (symbol >= u'0' && symbol <= u'9')
			? (symbol.unicode() - u'0')
			: (symbol.unicode() - u'a' + 10);
		value = (value * 16) + quint64(digit);
	}
	return kWallPaperIdOffset + value;
}

[[nodiscard]] MTPWallPaper PaperFrom(
		not_null<Main::Session*> session,
		const QJsonObject &object) {
	const auto pattern = object.value("kind").toString() == u"pattern"_q;
	const auto sha256 = pattern
		? QString::fromLatin1(QCryptographicHash::hash(
			object.value("pattern_id").toString().toUtf8(),
			QCryptographicHash::Sha256).toHex())
		: object.value("sha256").toString().toLower();
	const auto url = ClientFor(session).baseUrl().resolved(QUrl(object.value("url").toString())).toString();
	if (sha256.isEmpty() || object.value("url").toString().isEmpty()) {
		return MTPWallPaper();
	}
	auto width = object.value("width").toInt();
	auto height = object.value("height").toInt();
	if (width <= 0 || height <= 0) {
		width = height = pattern ? 1440 : 1;
	}
	const auto documentId = IdFor(sha256);
    const auto id = pattern ? IdFor(QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(QJsonObject{
            { "pattern_id", object.value("pattern_id") },
            { "colors", object.value("colors") },
            { "rotation", object.value("rotation") },
            { "intensity", object.value("intensity") },
        }).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())) : documentId;
    auto thumbnailUrl = QUrl(url);
    if (pattern) {
        auto query = QUrlQuery(thumbnailUrl);
        query.addQueryItem(u"format"_q, u"png"_q);
        thumbnailUrl.setQuery(query);
    }
	if (!pattern) StateFor(session).sha256ById[id] = sha256;
    StateFor(session).choicesById[id] = object;

	const auto attributes = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(pattern ? u"wallpaper.tgv"_q : u"wallpaper.jpg"_q)),
		MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)),
	};
	const auto document = MTP_document(
		MTP_flags(0),
		MTP_long(qint64(documentId)),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string(object.value("mime").toString(pattern ? u"application/x-tgwallpattern"_q : u"image/jpeg"_q)),
		MTP_long(object.value("size").toVariant().toLongLong()),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0),
		MTP_vector<MTPDocumentAttribute>(attributes));
	const auto data = session->data().processDocument(document);
	data->setContentUrl(url);
	data->updateThumbnails(
		InlineImageLocation(),
		ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ PlainUrlLocation{ thumbnailUrl.toString() } },
				width,
				height),
		},
		ImageWithLocation(),
		false);
	return MTP_wallPaper(
		MTP_long(qint64(id)),
		MTP_flags(pattern
			? (MTPDwallPaper::Flags(MTPDwallPaper::Flag::f_pattern)
				| (object.value("intensity").toInt() < 0
					? MTPDwallPaper::Flags(MTPDwallPaper::Flag::f_dark)
					: MTPDwallPaper::Flags()))
			: MTPDwallPaper::Flags()),
		MTP_long(0),
		MTP_string(pattern ? u"foxmes-pattern:"_q + object.value("pattern_id").toString() : sha256),
		document,
		MTPWallPaperSettings());
}

[[nodiscard]] QString Sha256Of(
		not_null<Main::Session*> session,
		const Data::WallPaper &paper) {
	const auto &map = StateFor(session).sha256ById;
	const auto i = map.find(paper.id());
	return (i == map.end()) ? QString() : i->second;
}

[[nodiscard]] int IntensityOf(const Data::WallPaper &paper) {
	return std::clamp(paper.patternIntensity(), -100, 100);
}

[[nodiscard]] Data::WallPaper WithChoice(
		const Data::WallPaper &paper,
		const QJsonObject &state) {
	return paper
		.withBlurred(state.value("blurred").toBool())
		.withPatternIntensity(state.value("intensity").toInt());
}


[[nodiscard]] QJsonObject ChoiceFrom(not_null<Main::Session*> session, const Data::WallPaper &paper) {
    if (IsNoBackground(paper)) return QJsonObject{{ "none", true }};
    if (Data::IsThemeWallPaper(paper) || Data::IsDefaultWallPaper(paper)) return QJsonObject{{ "wallpaper", QJsonValue(QJsonValue::Null) }};
    auto object = QJsonObject();
    if (const auto i = StateFor(session).choicesById.find(paper.id()); i != StateFor(session).choicesById.end()) object = i->second;
    auto colors = QJsonArray();
    for (const auto &color : paper.backgroundColors()) colors.push_back(int(color.rgb() & 0xffffff));
    if (object.isEmpty() && !colors.isEmpty()) object["kind"] = colors.size() > 1 ? "gradient" : "fill";
    if (!colors.isEmpty()) object["colors"] = colors;
    object["rotation"] = paper.gradientRotation();
    object["intensity"] = IntensityOf(paper);
    return QJsonObject{{ "wallpaper", object.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(object) }, { "blurred", paper.isBlurred() }, { "intensity", IntensityOf(paper) }, { "none", IsNoBackground(paper) }};
}
void SaveChoice(not_null<PeerData*> peer, QJsonObject body) {
    const auto session = &peer->session();
    const auto bridge = BridgeFor(session);
    const auto history = session->data().historyLoaded(peer->id);
    if (!bridge || !history) return;
    const auto weak = base::make_weak(session);
    bridge->resolveChatId(history, [weak, body](qint64 chatId) {
        const auto strong = weak.get();
        if (!strong || !BridgeFor(strong) || chatId <= 0) return;
        ClientFor(strong).setChatAppearance(chatId, body, [weak, chatId](QJsonDocument doc, QString error, int) {
            const auto strong = weak.get();
            if (!strong || !BridgeFor(strong)) return;
            if (error.isEmpty() && doc.isObject()) BridgeFor(strong)->applyAppearanceResult(chatId, doc.object());
            else { BridgeFor(strong)->reloadChats(); if (const auto window = strong->tryResolveWindow()) window->showToast(error); }
        });
    });
}
struct AppearanceTheme {
	QString path;
	bool night = false;
};

[[nodiscard]] std::optional<AppearanceTheme> ThemeNamed(const QString &name) {
	using Type = Window::Theme::EmbeddedType;
	const auto type = [&]() -> std::optional<Type> {
		if (name == u"classic"_q) {
			return Type::Default;
		} else if (name == u"day"_q) {
			return Type::DayBlue;
		} else if (name == u"tinted"_q) {
			return Type::Night;
		} else if (name == u"night"_q) {
			return Type::NightGreen;
		}
		return std::nullopt;
	}();
	if (!type) {
		return std::nullopt;
	}
	for (const auto &scheme : Window::Theme::EmbeddedThemes()) {
		if (scheme.type == *type) {
			return AppearanceTheme{
				scheme.path,
				(*type != Type::Default) && (*type != Type::DayBlue),
			};
		}
	}
	return std::nullopt;
}

void ApplyAppearanceDefault(
		not_null<Main::Session*> session,
		const QJsonObject &appearance) {
	if (appearance.isEmpty() || AppearanceDefaultApplied(session)) {
		return;
	}
	const auto theme = ThemeNamed(appearance.value("theme").toString());
	if (!theme) {
		return;
	}
	const auto background = Window::Theme::Background();
	const auto &object = background->themeObject();
	const auto untouched = !object.cloud.id
		&& (Window::Theme::IsNightMode()
			? ((object.pathAbsolute == Window::Theme::NightThemePath())
				&& Data::IsThemeWallPaper(background->paper()))
			: (object.pathAbsolute.isEmpty()
				&& Data::IsDefaultWallPaper(background->paper())));
	if (!untouched) {
		RememberAppearanceDefaultApplied(session);
		return;
	}
	auto &settings = Core::App().settings();
	if (settings.systemDarkModeEnabled()) {
		settings.setSystemDarkModeEnabled(false);
		Core::App().saveSettingsDelayed();
	}
	if (Window::Theme::IsNightMode() == theme->night) {
		Window::Theme::ApplyDefaultWithPath(theme->path);
	} else {
		Window::Theme::ToggleNightMode(theme->path);
	}
	Window::Theme::KeepApplied();
	if (appearance.value("wallpaper_none").toBool()
		&& !Data::IsThemeWallPaper(background->paper())) {
		background->set(Data::ThemeWallPaper());
	}
	RememberAppearanceDefaultApplied(session);
}

void RepairEmbeddedPalette(bool night) {
	const auto background = Window::Theme::Background();
	const auto paper = background->paper();
	const auto noBackground = IsNoBackground(paper);
	if (!noBackground && !Data::IsThemeWallPaper(paper)
		&& !Data::IsDefaultWallPaper(paper)) {
		auto preview = night
			? Window::Theme::PreviewFromFile(
				QByteArray(), Window::Theme::NightThemePath(), {})
			: std::make_unique<Window::Theme::Preview>();
		if (!preview) {
			return;
		}
		if (!night) {
			const auto accent = Core::App().settings().themesAccentColors().get(
				Window::Theme::EmbeddedType::Default);
			Window::Theme::PreparePaletteCallback(false, accent)(preview->instance.palette);
		}
		auto themeBackground = base::take(preview->instance.background);
		const auto tiled = preview->instance.tiled;
		Window::Theme::Apply(std::move(preview));
		Window::Theme::KeepApplied();
		background->setThemeData(std::move(themeBackground), tiled);
	} else {
		Window::Theme::ApplyDefaultWithPath(
			night ? Window::Theme::NightThemePath() : QString());
		Window::Theme::KeepApplied();
		if (noBackground) {
			background->set(NoBackgroundPaper());
		}
	}
}

void ApplyConfirmedNightMode(bool night) {
	if (Data::details::IsUninitializedWallPaper(Window::Theme::Background()->paper())) {
		return;
	}
	if (Window::Theme::IsNightMode() != night) {
		Window::Theme::ToggleNightMode();
		Window::Theme::KeepApplied();
	}
	if (Window::Theme::IsNightMode() != night) {
		return;
	}
	const auto &object = Window::Theme::Background()->themeObject();
	if (object.cloud.id) {
		return;
	}
	for (const auto &scheme : Window::Theme::EmbeddedThemes()) {
		if (scheme.path != object.pathAbsolute) {
			continue;
		}
		const auto dark = (scheme.type == Window::Theme::EmbeddedType::Night)
			|| (scheme.type == Window::Theme::EmbeddedType::NightGreen);
		// Старый стартовый вид мог сохранить тёмную палитру в дневном слоте.
		if (dark != night || (st::windowBg->c.lightnessF() < 0.5) != night) {
			RepairEmbeddedPalette(night);
		}
		return;
	}
}

} // namespace

void ApplyNightMode(
		not_null<Window::SessionController*> controller,
		bool night) {
	const auto weak = base::make_weak(controller);
	Window::Theme::ToggleNightModeWithConfirmation(&controller->window(), [=] {
		const auto strong = weak.get();
		if (!strong || Window::Theme::Background()->editingTheme()) {
			return;
		}
		RememberAppearanceDefaultApplied(&strong->session());
		ApplyConfirmedNightMode(night);
	});
}

void ToggleSystemNightMode(
		not_null<Window::SessionController*> controller) {
	auto &settings = Core::App().settings();
	const auto enabled = !settings.systemDarkModeEnabled();
	if (enabled && Window::Theme::Background()->editingTheme()) {
		controller->show(Ui::MakeInformBox(tr::lng_theme_editor_cant_change_theme()));
		return;
	}
	RememberAppearanceDefaultApplied(&controller->session());
	settings.setSystemDarkModeEnabled(enabled);
	Core::App().saveSettingsDelayed();
	if (enabled) {
		CheckSystemNightMode();
	} else if (!Window::Theme::Background()->editingTheme()) {
		ApplyConfirmedNightMode(Window::Theme::IsNightMode());
	}
}

void CheckSystemNightMode() {
	const auto &settings = Core::App().settings();
	const auto night = settings.systemDarkMode();
	const auto background = Window::Theme::Background();
	if (Data::details::IsUninitializedWallPaper(background->paper())
		|| background->editingTheme()) {
		return;
	}
	ApplyConfirmedNightMode(settings.systemDarkModeEnabled()
		? night.value_or(Window::Theme::IsNightMode())
		: Window::Theme::IsNightMode());
}

void ObserveNightMode(Fn<void(bool)> changed, rpl::lifetime &lifetime) {
	Window::Theme::IsNightModeValue() | rpl::on_next(std::move(changed), lifetime);
}

QJsonObject StateFromPaper(not_null<Main::Session*> session, const Data::WallPaper &paper) { return ChoiceFrom(session, paper); }

Data::WallPaper NoBackgroundPaper() {
    auto color = st::windowBg->c;
    const auto &object = Window::Theme::Background()->themeObject();
    auto native = Window::Theme::Instance();
    const auto loaded = object.content.isEmpty()
        ? (!object.pathAbsolute.isEmpty() && Window::Theme::LoadFromFile(object.pathAbsolute, &native, nullptr, nullptr))
        : Window::Theme::LoadFromContent(object.content, &native, nullptr);
    if (loaded && native.background.size() == QSize(1, 1)) color = native.background.pixelColor(0, 0);
    return Data::WallPaper(kNoBackgroundId).withBackgroundColors({ color });
}

bool IsNoBackground(const Data::WallPaper &paper) {
	return (paper.id() == kNoBackgroundId);
}

void RequestGallery(not_null<Main::Session*> session, Fn<void()> done) {
	if (!BridgeFor(session)) {
		return;
	}
	const auto weak = base::make_weak(session);
	ClientFor(session).chatThemes([weak, done = std::move(done)](
			QJsonDocument catalog,
			QString,
			int) {
		const auto strong = weak.get();
		if (!strong || !BridgeFor(strong)) return;
		ClientFor(strong).wallpapers([weak, catalog, done = std::move(done)](
				QJsonDocument doc,
				QString error,
				int) {
			const auto strong = weak.get();
			if (!strong || !BridgeFor(strong)) return;
			if (error.isEmpty() && doc.isObject()) {
				auto list = QVector<MTPWallPaper>();
				auto patterns = base::flat_set<QString>();
				for (const auto &item : catalog.object().value("items").toArray()) {
					for (const auto &entry : item.toObject().value("settings").toArray()) {
                        const auto base = entry.toObject().value("base_theme").toString();
                        const auto dark = base == u"night"_q || base == u"tinted"_q;
                        if (dark != Window::Theme::IsNightMode()) continue;
						const auto paper = entry.toObject().value("wallpaper").toObject();
						const auto id = paper.value("pattern_id").toString();
						if (paper.value("kind").toString() != u"pattern"_q
							|| id.isEmpty() || !patterns.emplace(id).second) continue;
						const auto native = NativePaper(strong, QJsonObject{
							{ "wallpaper", paper }, { "intensity", paper.value("intensity") },
						});
						if (native.type() == mtpc_wallPaper) list.push_back(native);
					}
				}
				for (const auto &value : doc.object().value("items").toArray()) {
					auto paper = PaperFrom(strong, value.toObject());
					if (paper.type() == mtpc_wallPaper) list.push_back(std::move(paper));
				}
				strong->data().updateWallpapers(MTP_account_wallPapers(
					MTP_long(0), MTP_vector<MTPWallPaper>(list)));
			}
			if (done) done();
		});
	});
}

void Remove(not_null<Main::Session*> session, const Data::WallPaper &paper) {
	const auto sha256 = Sha256Of(session, paper);
	if (sha256.isEmpty() || !BridgeFor(session)) {
		return;
	}
	ClientFor(session).deleteWallpaper(sha256, nullptr);
}

void Upload(
		not_null<Main::Session*> session,
		const QImage &image,
		Fn<void(std::optional<Data::WallPaper>)> done) {
	if (image.isNull() || !BridgeFor(session)) {
		if (done) done(std::nullopt);
		return;
	}
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	auto writer = QImageWriter(&buffer, "JPEG");
	writer.setQuality(kUploadJpegQuality);
	if (!writer.write(image)) {
		if (done) done(std::nullopt);
		return;
	}
	buffer.close();

	const auto weak = base::make_weak(session);
	ClientFor(session).uploadData(
		u"wallpaper.jpg"_q,
		bytes,
		u"image/jpeg"_q,
		0,
		false,
		[weak, done = std::move(done)](
				QJsonDocument doc,
				QString error,
				int) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			if (!error.isEmpty() || !doc.isObject()) {
				if (done) done(std::nullopt);
				return;
			}
			const auto data = doc.object().value("data").toObject();
			const auto paper = PaperFrom(strong, QJsonObject{
				{ "sha256", data.value("sha256") },
				{ "url", data.value("url") },
				{ "width", data.value("width") },
				{ "height", data.value("height") },
				{ "size", data.value("size") },
			});
			if (paper.type() != mtpc_wallPaper) {
				if (done) done(std::nullopt);
				return;
			}
			if (done) {
				done(Data::WallPaper::Create(strong, paper));
			}
		},
		nullptr,
		u"wallpaper"_q);
}

void SaveForPeer(not_null<PeerData*> peer, const Data::WallPaper &paper, bool both) {
    SaveChoice(peer, QJsonObject{{ "wallpaper", ChoiceFrom(&peer->session(), paper) }, { "for_both", both }});
}
void SetNoneForPeer(not_null<PeerData*> peer) {
    SaveChoice(peer, QJsonObject{{ "wallpaper", QJsonObject{{ "none", true }} }});
}
void ResetForPeer(not_null<PeerData*> peer) {
    SaveChoice(peer, QJsonObject{{ "reset_wallpaper", "self" }});
}
void RevertForPeer(not_null<PeerData*> peer) {
    SaveChoice(peer, QJsonObject{{ "revert", true }});
}
void SaveDefault(not_null<Main::Session*> session, const Data::WallPaper &paper) {
    if (!BridgeFor(session)) return;
    const auto weak = base::make_weak(session);
    const auto choice = ChoiceFrom(session, paper);
    ClientFor(session).setDefaultWallpaperState(choice, [weak](QJsonDocument doc, QString error, int) {
        const auto strong = weak.get();
        if (!strong || !BridgeFor(strong)) return;
        if (error.isEmpty() && doc.isObject()) ApplyDefault(strong, doc.object());
        else { RequestDefault(strong); if (const auto window = strong->tryResolveWindow()) window->showToast(error); }
    });
}

bool ChooseNoBackground(
		not_null<Window::SessionController*> controller,
		PeerData *forPeer,
		const Data::WallPaper &paper) {
	if (!IsNoBackground(paper)) {
		return false;
	}
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller.get());
	const auto peerId = forPeer ? forPeer->id : PeerId();
	crl::on_main(session, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		if (peerId) {
			const auto peer = strong->session().data().peerLoaded(peerId);
			if (!peer) {
				return;
			}
			peer->setWallPaper(NoBackgroundPaper());
			SetNoneForPeer(peer);
			strong->finishChatThemeEdit(peer);
			return;
		}
		SaveDefault(session, NoBackgroundPaper());
	});
	return true;
}

void ApplyDefault(not_null<Main::Session*> session, const QJsonObject &state) {
    auto &cache = StateFor(session);
    const auto revision = state.value("settings_revision").toVariant().toLongLong();
    if (revision < cache.defaultRevision) return;
    cache.defaultRevision = revision;
    const auto generation = ++cache.defaultGeneration;
    if (state.value("appearance").isObject()) {
        ApplyAppearanceDefault(session, state.value("appearance").toObject());
        return;
    }
    if (!state.value("wallpaper").isObject()) {
        Window::Theme::Background()->set(state.value("none").toBool() ? NoBackgroundPaper() : Data::ThemeWallPaper());
        return;
    }
    const auto paper = Data::WallPaper::Create(session, NativePaper(session, state));
    if (!paper) return;
    const auto document = paper->document();
    if (!document) {
        Window::Theme::Background()->set(*paper);
        return;
    }
    const auto object = state.value("wallpaper").toObject();
    const auto url = ClientFor(session).baseUrl().resolved(QUrl(object.value("url").toString()));
    const auto weak = base::make_weak(session);
    ClientFor(session).downloadUrl(url, [weak, paper = *paper, generation](QByteArray bytes, QString error, int) {
        const auto strong = weak.get();
        if (!strong || !BridgeFor(strong) || !error.isEmpty() || StateFor(strong).defaultGeneration != generation) return;
        paper.document()->setDataAndCache(bytes);
        const auto gzipSvg = paper.document()->isPatternWallPaperSVG();
        crl::async([weak, paper, generation, gzipSvg, bytes = std::move(bytes)] {
            auto image = Ui::ReadBackgroundImage(QString(), bytes, gzipSvg).image;
            crl::on_main([weak, paper, generation, image = std::move(image)]() mutable {
                const auto strong = weak.get();
                if (!strong || !BridgeFor(strong) || StateFor(strong).defaultGeneration != generation || image.isNull()) return;
                Window::Theme::Background()->set(paper, std::move(image));
            });
        });
    });
}

MTPWallPaper NativePaper(not_null<Main::Session*> session, const QJsonObject &state) {
    const auto object = state.value("wallpaper").toObject();
    const auto kind = object.value("kind").toString();
    if (kind == u"fill"_q || kind == u"gradient"_q) {
        const auto colors = object.value("colors").toArray();
        using Flag = MTPDwallPaperSettings::Flag;
        auto flags = MTPDwallPaperSettings::Flags(Flag::f_background_color);
        if (colors.size() > 1) flags |= Flag::f_second_background_color;
        if (colors.size() > 2) flags |= Flag::f_third_background_color;
        if (colors.size() > 3) flags |= Flag::f_fourth_background_color;
        if (object.value("motion").toBool()) flags |= Flag::f_motion;
        const auto color = [&](int index) { return MTP_int(index < colors.size() ? colors[index].toInt() : 0); };
        return MTP_wallPaperNoFile(MTP_long(0), MTP_flags(MTPDwallPaperNoFile::Flag::f_settings),
            MTP_wallPaperSettings(MTP_flags(flags), color(0), color(1), color(2), color(3), MTP_int(0), MTP_int(object.value("rotation").toInt()), MTPstring()));
    }
    if (object.isEmpty()) {
        return MTP_wallPaperNoFile(MTP_long(qint64(kNoBackgroundId)), MTP_flags(MTPDwallPaperNoFile::Flags()), MTPWallPaperSettings());
    }
    auto paper = PaperFrom(session, object);
    if (paper.type() != mtpc_wallPaper) return paper;
    const auto &data = paper.c_wallPaper();
    auto parsed = Data::WallPaper::Create(session, paper);
    if (!parsed) return MTPWallPaper();
    if (kind == u"pattern"_q) {
        auto colors = std::vector<QColor>();
        for (const auto &color : object.value("colors").toArray()) colors.push_back(QColor::fromRgb(color.toInt()));
        parsed = parsed->withBackgroundColors(colors).withGradientRotation(object.value("rotation").toInt()).withPatternIntensity(state.value("intensity").toInt());
    }
    return MTP_wallPaper(data.vid(), MTP_flags(data.vflags().v | MTPDwallPaper::Flag::f_settings),
        data.vaccess_hash(), data.vslug(), data.vdocument(), WithChoice(*parsed, state).mtpSettings());
}

void RequestDefault(not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	ClientFor(session).defaultWallpaper([weak](
			QJsonDocument doc,
			QString error,
			int) {
		const auto strong = weak.get();
		if (!strong || !error.isEmpty() || !doc.isObject()) {
			return;
		}
        ApplyDefault(strong, doc.object());
	});
}

void ApplyForPeer(not_null<PeerData*> peer, const QJsonObject &wallpaper) {
	const auto session = &peer->session();
	if (wallpaper.value("none").toBool()) {
		if (!peer->wallPaper() || !IsNoBackground(*peer->wallPaper())) {
			peer->setWallPaper(NoBackgroundPaper());
		}
		return;
	}
	if (!wallpaper.value("wallpaper").isObject()) {
		if (peer->wallPaper()) {
			peer->setWallPaper({});
		}
		return;
	}
	const auto mtp = NativePaper(session, wallpaper);
	if (mtp.type() != mtpc_wallPaper && mtp.type() != mtpc_wallPaperNoFile) {
		return;
	}
	const auto paper = Data::WallPaper::Create(session, mtp);
	if (!paper) {
		return;
	}
	const auto ready = WithChoice(*paper, wallpaper);
	if (const auto current = peer->wallPaper()) {
		if (current->equals(ready)) {
			return;
		}
	}
	peer->setWallPaper(ready);
	ready.loadDocument();
}

void ClearSession(not_null<Main::Session*> session) {
	States().remove(session);
}

}
