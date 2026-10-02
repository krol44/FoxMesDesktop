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
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_wall_paper.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "window/themes/window_theme.h"
#include "window/themes/window_themes_embedded.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtGui/QImageWriter>

namespace CustomBackend::Wallpapers {
namespace {

constexpr auto kWallPaperIdOffset = quint64(4000000000000000ULL);

constexpr auto kUploadJpegQuality = 87;

constexpr auto kNoBackgroundId = quint64(4000000000000001ULL);

struct State {
	base::flat_map<quint64, QString> sha256ById;
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
	const auto sha256 = object.value("sha256").toString().toLower();
	const auto url = object.value("url").toString().trimmed();
	if (sha256.isEmpty() || url.isEmpty()) {
		return MTPWallPaper();
	}
	auto width = object.value("width").toInt();
	auto height = object.value("height").toInt();
	if (width <= 0 || height <= 0) {
		width = height = 1;
	}
	const auto id = IdFor(sha256);
	StateFor(session).sha256ById[id] = sha256;

	const auto attributes = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(u"wallpaper.jpg"_q)),
		MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)),
	};
	const auto document = MTP_document(
		MTP_flags(0),
		MTP_long(qint64(id)),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string(u"image/jpeg"_q),
		MTP_long(object.value("size").toVariant().toLongLong()),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0),
		MTP_vector<MTPDocumentAttribute>(attributes));
	const auto data = session->data().document(qint64(id));
	data->setContentUrl(url);
	data->updateThumbnails(
		InlineImageLocation(),
		ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ PlainUrlLocation{ url } },
				width,
				height),
		},
		ImageWithLocation(),
		false);
	session->data().processDocument(document);

	return MTP_wallPaper(
		MTP_long(qint64(id)),
		MTP_flags(MTPDwallPaper::Flags()),
		MTP_long(0),
		MTP_string(sha256),
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
	return std::clamp(paper.patternIntensity(), 0, 100);
}

[[nodiscard]] Data::WallPaper WithChoice(
		const Data::WallPaper &paper,
		const QJsonObject &state) {
	return paper
		.withBlurred(state.value("blurred").toBool())
		.withPatternIntensity(state.value("intensity").toInt());
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

} // namespace

Data::WallPaper NoBackgroundPaper() {
	return Data::WallPaper(kNoBackgroundId).withBackgroundColors({
		st::windowBg->c,
	});
}

bool IsNoBackground(const Data::WallPaper &paper) {
	return (paper.id() == kNoBackgroundId);
}

void RequestGallery(not_null<Main::Session*> session, Fn<void()> done) {
	if (!BridgeFor(session)) {
		return;
	}
	const auto weak = base::make_weak(session);
	ClientFor(session).wallpapers([weak, done = std::move(done)](
			QJsonDocument doc,
			QString error,
			int) {
		const auto strong = weak.get();
		if (!strong || !error.isEmpty() || !doc.isObject()) {
			return;
		}
		auto list = QVector<MTPWallPaper>();
		const auto items = doc.object().value("items").toArray();
		list.reserve(items.size());
		for (const auto &value : items) {
			auto paper = PaperFrom(strong, value.toObject());
			if (paper.type() == mtpc_wallPaper) {
				list.push_back(std::move(paper));
			}
		}
		strong->data().updateWallpapers(MTP_account_wallPapers(
			MTP_long(0),
			MTP_vector<MTPWallPaper>(list)));
		if (done) {
			done();
		}
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

void SaveForPeer(
		not_null<PeerData*> peer,
		const Data::WallPaper &paper,
		bool both) {
	const auto session = &peer->session();
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	const auto sha256 = Sha256Of(session, paper);
	if (sha256.isEmpty()) {
		return;
	}
	const auto history = session->data().historyLoaded(peer->id);
	if (!history) {
		return;
	}
	const auto blurred = paper.isBlurred();
	const auto intensity = IntensityOf(paper);
	const auto weak = base::make_weak(session);
	bridge->resolveChatId(history, [weak, sha256, blurred, intensity, both](
			qint64 chatId) {
		const auto strong = weak.get();
		if (!strong || chatId <= 0 || !BridgeFor(strong)) {
			return;
		}
		ClientFor(strong).setChatWallpaper(
			chatId,
			sha256,
			blurred,
			intensity,
			both,
			QString(),
			nullptr);
	});
}

void SetNoneForPeer(not_null<PeerData*> peer) {
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
	bridge->resolveChatId(history, [weak](qint64 chatId) {
		const auto strong = weak.get();
		if (!strong || chatId <= 0 || !BridgeFor(strong)) {
			return;
		}
		ClientFor(strong).setChatNoWallpaper(chatId, QString(), nullptr);
	});
}

void ResetForPeer(not_null<PeerData*> peer) {
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
	bridge->resolveChatId(history, [weak](qint64 chatId) {
		const auto strong = weak.get();
		if (!strong || chatId <= 0 || !BridgeFor(strong)) {
			return;
		}
		ClientFor(strong).setChatWallpaper(
			chatId,
			QString(),
			false,
			0,
			false,
			QString(),
			nullptr);
	});
}

void SaveDefault(
		not_null<Main::Session*> session,
		const Data::WallPaper &paper) {
	if (!BridgeFor(session)) {
		return;
	}
	const auto sha256 = Sha256Of(session, paper);
	if (sha256.isEmpty()) {
		return;
	}
	ClientFor(session).setDefaultWallpaper(
		sha256,
		paper.isBlurred(),
		IntensityOf(paper),
		QString(),
		nullptr);
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
		Window::Theme::Background()->set(Data::ThemeWallPaper());
		if (BridgeFor(session)) {
			ClientFor(session).setDefaultWallpaper(
				QString(),
				false,
				0,
				QString(),
				nullptr);
		}
	});
	return true;
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
		const auto state = doc.object();
		ApplyAppearanceDefault(strong, state.value("appearance").toObject());
		if (!state.value("wallpaper").isObject()) {
			return;
		}
		const auto mtp = PaperFrom(strong, state.value("wallpaper").toObject());
		if (mtp.type() != mtpc_wallPaper) {
			return;
		}
		const auto paper = Data::WallPaper::Create(strong, mtp);
		if (!paper) {
			return;
		}
		const auto ready = WithChoice(*paper, state);
		Window::Theme::Background()->set(ready);
		ready.loadDocument();
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
	const auto mtp = PaperFrom(session, wallpaper.value("wallpaper").toObject());
	if (mtp.type() != mtpc_wallPaper) {
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
