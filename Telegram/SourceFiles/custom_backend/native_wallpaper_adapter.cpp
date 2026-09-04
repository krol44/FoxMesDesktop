/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_wallpaper_adapter.h"

#include "base/unixtime.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_wall_paper.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "window/themes/window_theme.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtGui/QImageWriter>

namespace CustomBackend::Wallpapers {
namespace {

// Wallpaper documents live in their own id range, disjoint from attachments,
// link previews, saved GIFs and chat theme gradients: all of them end up in the
// same Data::Session maps.
constexpr auto kWallPaperIdOffset = quint64(4000000000000000ULL);

// What a picture is uploaded as. The gallery is small and the file is shown
// full-screen behind a chat, so quality matters more than bytes here.
constexpr auto kUploadJpegQuality = 87;

// The id of the "no background" paper. Its own, so the gallery can tell it from
// every other cell and mark it as the current one.
constexpr auto kNoBackgroundId = quint64(4000000000000001ULL);

struct State {
	// The content address behind each paper id. A WallPaper carries neither the
	// sha256 nor a readable url, and the server names a picture by its sha256 -
	// so the mapping is remembered as each paper is built.
	base::flat_map<quint64, QString> sha256ById;
};

base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	return States()[session];
}

// A stable id for one picture, derived from its content address: WallPaper is
// keyed by id, so a fresh id on every gallery load would rebuild the chat theme
// and lose the "this one is selected" mark on every open.
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

// One gallery entry as an MTPWallPaper.
//
// Built as MTP rather than as a Data::WallPaper because that is the shape
// Data::Session::updateWallpapers() takes, and going through it keeps the whole
// list exactly as upstream builds it - the legacy defaults it prepends, the
// ordering it applies. A converter of our own would have to reproduce all of
// that and would drift from it.
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

	// The document is what carries the pixels: BackgroundPreviewBox and
	// SessionController::cacheChatTheme() both read them out of
	// paper.document(), and DocumentData::checkWallPaperProperties() refuses a
	// document with no thumbnail or no dimensions - WallPaper::Create() then
	// answers nullopt and the picture silently never appears.
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
	// Pointed at its bytes before processDocument() applies the fields:
	// applying repaints whatever already shows the document, and that repaint
	// is what starts the download. A document given its url afterwards can only
	// build an MTProto loader, which never finishes under the bridge.
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

// The dimming slider. Upstream stores it as the pattern intensity of the paper
// (BackgroundPreviewBox::_dimmingIntensity), so that is where we read it from
// and where we put it back.
[[nodiscard]] int IntensityOf(const Data::WallPaper &paper) {
	return std::clamp(paper.patternIntensity(), 0, 100);
}

// Restores the two per-choice settings the server stores next to the sha256.
[[nodiscard]] Data::WallPaper WithChoice(
		const Data::WallPaper &paper,
		const QJsonObject &state) {
	return paper
		.withBlurred(state.value("blurred").toBool())
		.withPatternIntensity(state.value("intensity").toInt());
}

} // namespace

Data::WallPaper NoBackgroundPaper() {
	// A colours-only paper, which is how Telegram represents a solid or
	// gradient background (WallPaper::FromColorsSlug). Everything downstream
	// already handles that shape: the gallery paints a cell from the colours
	// (BackgroundBox::Inner::validatePaperThumbnail), and a chat renders a
	// flat fill instead of trying to prepare an image that does not exist.
	//
	// This is what "no background" has to be for one chat. Data::ThemeWallPaper
	// would be the honest marker, but it carries no colours and no document, so
	// a chat given it renders from an unprepared image - which is where the
	// smearing and repeated tiles came from.
	//
	// The colour is read from the palette on every build rather than stored:
	// the choice is "no background", and what that looks like follows the
	// theme.
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
		// The hash stays 0: it is the server's "nothing changed" token and we
		// have no counterpart for it, so every answer is a full list.
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
	// The wallpaper lane, not the chat one: the picture belongs to the person
	// and is never referenced from a message, so the server keeps it active
	// instead of waiting for a send to confirm it.
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

// The chat shows the flat theme background whatever the account default is.
// Distinct from ResetForPeer(), which only drops the chat's own picture and
// lets that default take over again.
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
		// An empty sha256 is the documented reset: the chat follows the
		// per-user default again.
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
	// Applied on the next loop pass, never here. BackgroundBox::chosen() is
	// called from the mouse handler with a const reference to an element of
	// BackgroundBox::Inner::_papers, and applying a background synchronously
	// makes ChatBackground fire its update - which has the box re-sort and
	// rebuild that very vector while the reference into it is still live.
	//
	// Deferring also lets the caller close its box first, which is what it
	// does the moment this answers true.
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
		// A removal, not a new background: the account goes back to the state
		// it had before anything was ever set, which is the background the
		// theme itself carries. ThemeWallPaper is exactly that marker, and it
		// is what ChatBackground::initialRead() falls back to on a fresh
		// install - it is only unusable as a *per-chat* paper, where nothing
		// can prepare an image for it.
		Window::Theme::Background()->set(Data::ThemeWallPaper());
		if (BridgeFor(session)) {
			// The empty sha256 is the documented reset of the per-user
			// default.
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
	if (!BridgeFor(session)) {
		return;
	}
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
		if (!state.value("wallpaper").isObject()) {
			// An explicit null means "no default"; the local background is
			// left as it is rather than reset, because the user may have
			// picked a file that never reached the server.
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
		// Set before the pixels arrive, exactly as upstream does when it reads
		// a stored background at startup: the document download fills them in
		// and the background repaints itself.
		Window::Theme::Background()->set(ready);
		ready.loadDocument();
	});
}

// wallpaper is the same state object GET /wallpaper answers: the picture plus
// the two settings that belong to the choice rather than to the file.
void ApplyForPeer(not_null<PeerData*> peer, const QJsonObject &wallpaper) {
	const auto session = &peer->session();
	if (wallpaper.value("none").toBool()) {
		// Rebuilt from the current palette rather than stored: the choice is
		// "no background", and what that looks like follows the theme.
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

} // namespace CustomBackend::Wallpapers
