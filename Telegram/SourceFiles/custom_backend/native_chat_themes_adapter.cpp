/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_chat_themes_adapter.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_cloud_themes.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_wall_paper.h"
#include "history/history.h"
#include "main/main_session.h"

#include <QtCore/QFile>

namespace CustomBackend::ChatThemes {
namespace {

// Ids of our own. They only have to be non-zero and stable: upstream keys a
// cached Ui::ChatTheme by ChatThemeKey{id, dark}, so a theme that changed id
// between two fills would be re-rendered from scratch every time the picker
// opened. Nothing sends them anywhere - the wire protocol names a theme by its
// emoji.
constexpr auto kThemeIdBase = uint64(0xF0DE000000000000ULL);

// The gradient behind each theme is a WallPaper, and a WallPaper is keyed by
// its id too - in the same space as the wallpapers of the gallery, so this
// base is disjoint from kWallPaperMediaIdOffset in the wallpaper adapter.
constexpr auto kThemePaperIdBase = uint64(0xF0DF000000000000ULL);

// The shared pattern document. One id for all of them: every theme draws the
// same doodles and differs only in the colours under them.
constexpr auto kPatternDocumentId = uint64(0xF0DD000000000001ULL);

// Declared, not measured: the pattern is a vector image and the renderer scales
// it to the window. It only has to be a valid, non-empty size that
// checkWallPaperProperties() accepts.
constexpr auto kPatternSide = 1440;

// How strongly the doodles show through. Telegram's own chat themes sit around
// here; lower washes the pattern out, higher makes it fight the text.
constexpr auto kPatternIntensity = 50;

// The colour encoding MTPWallPaperSettings uses. data_wall_paper.cpp has the
// same helper as a file-local static, so it cannot be shared.
[[nodiscard]] quint32 SerializeColor(const QColor &color) {
	return (quint32(std::clamp(color.red(), 0, 255)) << 16)
		| (quint32(std::clamp(color.green(), 0, 255)) << 8)
		| quint32(std::clamp(color.blue(), 0, 255));
}

// The media view that owns the pattern bytes. Nothing uploads or downloads this
// document, so this is the only copy of its content and it has to outlive every
// paper built from it.
base::flat_map<
	not_null<Main::Session*>,
	std::shared_ptr<Data::DocumentMedia>> &PatternMedia() {
	static auto value = base::flat_map<
		not_null<Main::Session*>,
		std::shared_ptr<Data::DocumentMedia>>();
	return value;
}

struct Palette {
	// The background gradient. Two to four colours, blended by the same code
	// that draws a Telegram gradient wallpaper.
	std::vector<QColor> background;
	// The outgoing bubble. Two colours make it a gradient, which is what the
	// picker previews and what the bubble is painted with.
	std::vector<QColor> outgoing;
	QColor accent;
};

struct ThemeSpec {
	QString emoticon;
	Palette light;
	Palette dark;
};

// The catalog. Emoji rather than names because that is what the contract
// stores and what the picker draws on each card - and because
// CloudThemes::themeForToken() resolves a stored choice through
// Ui::Emoji::Find(), so anything that is not a plain Unicode emoji would never
// resolve back.
//
// Each background is a gradient of four shades of the theme colour with the
// bundled doodle pattern drawn over it, and each outgoing bubble is a two-stop
// gradient of the same hue - the shape a Telegram chat theme has.
[[nodiscard]] const std::vector<ThemeSpec> &Catalog() {
	static const auto value = std::vector<ThemeSpec>{
		{
			u"\U0001F425"_q, // hatching chick - green
			{ { QColor(0xD6, 0xEE, 0xD2), QColor(0xB4, 0xDE, 0xB1),
				QColor(0xC8, 0xE8, 0xC3), QColor(0x9F, 0xD2, 0xA2) },
			  { QColor(0x6F, 0xC2, 0x76), QColor(0x3E, 0x9E, 0x54) },
			  QColor(0x3E, 0x9E, 0x54) },
			{ { QColor(0x0D, 0x18, 0x0F), QColor(0x13, 0x26, 0x18),
				QColor(0x10, 0x1F, 0x13), QColor(0x18, 0x30, 0x1D) },
			  { QColor(0x4C, 0xA1, 0x5C), QColor(0x2E, 0x76, 0x3E) },
			  QColor(0x6F, 0xC2, 0x76) },
		},
		{
			u"\U000026C4"_q, // snowman - blue
			{ { QColor(0xCF, 0xE6, 0xF6), QColor(0xA6, 0xCF, 0xEC),
				QColor(0xBB, 0xDB, 0xF1), QColor(0x8B, 0xC0, 0xE6) },
			  { QColor(0x4A, 0xA8, 0xE0), QColor(0x2C, 0x7C, 0xB8) },
			  QColor(0x2C, 0x7C, 0xB8) },
			{ { QColor(0x0A, 0x14, 0x1C), QColor(0x0F, 0x22, 0x2F),
				QColor(0x0C, 0x1B, 0x25), QColor(0x13, 0x2C, 0x3C) },
			  { QColor(0x36, 0x8F, 0xC8), QColor(0x22, 0x63, 0x93) },
			  QColor(0x5C, 0xB2, 0xE4) },
		},
		{
			u"\U0001F48E"_q, // gem - blue to purple
			{ { QColor(0xD3, 0xDE, 0xF7), QColor(0xB6, 0xB9, 0xEE),
				QColor(0xC5, 0xCB, 0xF3), QColor(0xA5, 0xA6, 0xE8) },
			  { QColor(0x5A, 0x8F, 0xDE), QColor(0x7A, 0x5C, 0xC8) },
			  QColor(0x6A, 0x74, 0xD4) },
			{ { QColor(0x0C, 0x0F, 0x1D), QColor(0x14, 0x17, 0x30),
				QColor(0x0F, 0x12, 0x25), QColor(0x1B, 0x1D, 0x3D) },
			  { QColor(0x3F, 0x6F, 0xBE), QColor(0x5E, 0x45, 0xA6) },
			  QColor(0x8A, 0x92, 0xE4) },
		},
		{
			u"\U0001F469"_q, // woman - green
			{ { QColor(0xD9, 0xEC, 0xD9), QColor(0xB2, 0xD9, 0xC0),
				QColor(0xC7, 0xE5, 0xCC), QColor(0x9C, 0xCE, 0xB4) },
			  { QColor(0x62, 0xBB, 0x8C), QColor(0x38, 0x93, 0x69) },
			  QColor(0x38, 0x93, 0x69) },
			{ { QColor(0x0B, 0x17, 0x13), QColor(0x11, 0x26, 0x1F),
				QColor(0x0E, 0x1E, 0x19), QColor(0x16, 0x30, 0x28) },
			  { QColor(0x44, 0x9C, 0x74), QColor(0x2A, 0x71, 0x53) },
			  QColor(0x6C, 0xC0, 0x96) },
		},
		{
			u"\U0001F337"_q, // tulip - orange to red
			{ { QColor(0xF9, 0xDF, 0xD2), QColor(0xF2, 0xBA, 0xAE),
				QColor(0xF6, 0xCE, 0xC1), QColor(0xEC, 0xA4, 0x9C) },
			  { QColor(0xE8, 0x8A, 0x5E), QColor(0xD1, 0x59, 0x5C) },
			  QColor(0xD1, 0x59, 0x5C) },
			{ { QColor(0x1B, 0x0E, 0x0C), QColor(0x2D, 0x16, 0x13),
				QColor(0x22, 0x11, 0x0F), QColor(0x38, 0x1C, 0x18) },
			  { QColor(0xC0, 0x6A, 0x45), QColor(0xA1, 0x40, 0x44) },
			  QColor(0xE8, 0x8A, 0x5E) },
		},
		{
			u"\U0001F49C"_q, // purple heart - purple to pink
			{ { QColor(0xE7, 0xD8, 0xF3), QColor(0xDB, 0xB4, 0xE8),
				QColor(0xE1, 0xC7, 0xEE), QColor(0xD3, 0xA2, 0xE2) },
			  { QColor(0xA9, 0x6B, 0xD8), QColor(0xD2, 0x62, 0xB0) },
			  QColor(0xA9, 0x6B, 0xD8) },
			{ { QColor(0x16, 0x0D, 0x1D), QColor(0x25, 0x14, 0x30),
				QColor(0x1B, 0x10, 0x25), QColor(0x2E, 0x19, 0x3C) },
			  { QColor(0x84, 0x4F, 0xAC), QColor(0xA4, 0x47, 0x88) },
			  QColor(0xC0, 0x8B, 0xE4) },
		},
		{
			u"\U0001F384"_q, // christmas tree - amber
			{ { QColor(0xFA, 0xE8, 0xCE), QColor(0xF3, 0xD2, 0xA1),
				QColor(0xF7, 0xDE, 0xB8), QColor(0xEE, 0xC5, 0x8A) },
			  { QColor(0xE0, 0xA6, 0x4E), QColor(0xC0, 0x7F, 0x2E) },
			  QColor(0xC0, 0x7F, 0x2E) },
			{ { QColor(0x1A, 0x14, 0x0A), QColor(0x2B, 0x21, 0x10),
				QColor(0x20, 0x19, 0x0D), QColor(0x36, 0x29, 0x15) },
			  { QColor(0xB5, 0x84, 0x3A), QColor(0x8E, 0x62, 0x25) },
			  QColor(0xE0, 0xA6, 0x4E) },
		},
		{
			u"\U0001F3AE"_q, // video game - blue to violet
			{ { QColor(0xD5, 0xDD, 0xF5), QColor(0xBC, 0xC0, 0xEF),
				QColor(0xC8, 0xCE, 0xF2), QColor(0xAE, 0xAE, 0xEA) },
			  { QColor(0x53, 0x92, 0xDD), QColor(0x8A, 0x5C, 0xD0) },
			  QColor(0x6E, 0x77, 0xD8) },
			{ { QColor(0x0B, 0x0E, 0x1E), QColor(0x15, 0x16, 0x33),
				QColor(0x0F, 0x11, 0x26), QColor(0x1C, 0x1B, 0x41) },
			  { QColor(0x3A, 0x72, 0xC4), QColor(0x68, 0x44, 0xB0) },
			  QColor(0x93, 0x99, 0xE8) },
		},
	};
	return value;
}

// The doodle pattern drawn over every theme background. It is the one that
// ships with the client (the default Telegram background), so no theme needs an
// asset of its own: only the colours under it differ.
//
// One document for all sixteen papers - eight themes times light and dark. It
// is built once per session and kept alive by the media view below, because
// nothing ever uploads or downloads it and that view holds the only copy of
// its bytes.
[[nodiscard]] DocumentData *PatternDocument(
		not_null<Main::Session*> session) {
	static auto cache = base::flat_map<not_null<Main::Session*>, DocumentData*>();
	if (const auto i = cache.find(session); i != cache.end()) {
		return i->second;
	}
	auto file = QFile(u":/gui/art/background.tgv"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return nullptr;
	}
	const auto bytes = file.readAll();
	if (bytes.isEmpty()) {
		return nullptr;
	}
	// The mime is what makes DocumentData::isPatternWallPaperSVG() true, and
	// that is what tells the background renderer to gunzip the bytes and paint
	// them as a tinted pattern instead of a picture.
	const auto attributes = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(u"pattern.tgv"_q)),
		MTP_documentAttributeImageSize(
			MTP_int(kPatternSide),
			MTP_int(kPatternSide)),
	};
	const auto document = MTP_document(
		MTP_flags(0),
		MTP_long(qint64(kPatternDocumentId)),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string(u"application/x-tgwallpattern"_q),
		MTP_long(bytes.size()),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0),
		MTP_vector<MTPDocumentAttribute>(attributes));
	const auto data = session->data().document(qint64(kPatternDocumentId));
	// A thumbnail is mandatory: DocumentData::checkWallPaperProperties()
	// refuses a document without one, and WallPaper::Create() then answers
	// nullopt. The bytes are already here, so the location is an in-memory one
	// rather than anything that would have to be fetched.
	data->updateThumbnails(
		InlineImageLocation(),
		ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ InMemoryLocation{ bytes } },
				kPatternSide,
				kPatternSide),
			// Declaration order: ImageWithLocation lists bytes before
			// bytesCount, and C++20 requires designators to follow it. Clang
			// only warns, GCC rejects it outright.
			.bytes = bytes,
			.bytesCount = int(bytes.size()),
		},
		ImageWithLocation(),
		false);
	session->data().processDocument(document);
	auto media = data->createMediaView();
	media->setBytes(bytes);
	PatternMedia()[session] = std::move(media);
	cache[session] = data;
	return data;
}

[[nodiscard]] Data::CloudTheme::Settings SettingsFrom(
		not_null<Main::Session*> session,
		const Palette &palette,
		quint64 paperId) {
	const auto document = PatternDocument(session);
	if (!document) {
		// No pattern available: a plain gradient is still a usable theme, and
		// it is what the colours alone describe.
		return Data::CloudTheme::Settings{
			.paper = Data::WallPaper(paperId).withBackgroundColors(
				palette.background),
			.accentColor = palette.accent,
			.outgoingMessagesColors = palette.outgoing,
		};
	}
	// Built through MTP because the pattern flag and the gradient colours have
	// no public setters: WallPaper::Create() reads the colours out of the
	// settings only when the paper is a pattern (data_wall_paper.cpp), so the
	// flag and the colours have to arrive together.
	using Flag = MTPDwallPaperSettings::Flag;
	const auto color = [&](int index) {
		return (int(palette.background.size()) > index)
			? MTP_int(SerializeColor(palette.background[index]))
			: MTP_int(0);
	};
	const auto settings = MTP_wallPaperSettings(
		MTP_flags(Flag::f_intensity
			| Flag::f_background_color
			| Flag::f_second_background_color
			| Flag::f_third_background_color
			| Flag::f_fourth_background_color),
		color(0),
		color(1),
		color(2),
		color(3),
		MTP_int(kPatternIntensity),
		MTP_int(0),
		MTPstring());
	auto paper = Data::WallPaper::Create(session, MTP_wallPaper(
		MTP_long(qint64(paperId)),
		// f_settings as well as f_pattern: the settings field is optional, and
		// without its flag vsettings() is empty, so WallPaper::Create() never
		// reads the gradient - which left every theme with no colours and no
		// preview to draw.
		MTP_flags(MTPDwallPaper::Flag::f_pattern
			| MTPDwallPaper::Flag::f_settings),
		MTP_long(0),
		MTP_string(QString::number(paperId)),
		MTP_document(
			MTP_flags(0),
			MTP_long(document->id),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(base::unixtime::now()),
			MTP_string(u"application/x-tgwallpattern"_q),
			MTP_long(document->size),
			MTPVector<MTPPhotoSize>(),
			MTPVector<MTPVideoSize>(),
			MTP_int(0),
			MTP_vector<MTPDocumentAttribute>(QVector<MTPDocumentAttribute>{
				MTP_documentAttributeFilename(MTP_string(u"pattern.tgv"_q)),
				MTP_documentAttributeImageSize(
					MTP_int(kPatternSide),
					MTP_int(kPatternSide)),
			})),
		settings).c_wallPaper());
	return Data::CloudTheme::Settings{
		.paper = paper
			? *paper
			: Data::WallPaper(paperId).withBackgroundColors(
				palette.background),
		.accentColor = palette.accent,
		.outgoingMessagesColors = palette.outgoing,
	};
}

[[nodiscard]] std::vector<Data::CloudTheme> BuildCatalog(
		not_null<Main::Session*> session) {
	const auto &specs = Catalog();
	auto result = std::vector<Data::CloudTheme>();
	result.reserve(specs.size());
	auto index = quint64(0);
	for (const auto &spec : specs) {
		const auto id = kThemeIdBase + (++index);
		auto theme = Data::CloudTheme();
		theme.id = id;
		theme.emoticon = spec.emoticon;
		using Type = Data::CloudThemeType;
		// The two gradients need ids of their own, not one derived from the
		// other by arithmetic: a WallPaper is keyed by its id, and two papers
		// sharing one would have the light variant cached for the dark.
		const auto lightPaperId = kThemePaperIdBase + index * 2;
		const auto darkPaperId = lightPaperId + 1;
		// Both types are always filled: the picker skips a theme that has no
		// settings for the current one, so a light-only catalog would empty
		// itself the moment the user switched to night mode.
		theme.settings.emplace(
			Type::Light,
			SettingsFrom(session, spec.light, lightPaperId));
		theme.settings.emplace(
			Type::Dark,
			SettingsFrom(session, spec.dark, darkPaperId));
		result.push_back(std::move(theme));
	}
	return result;
}

} // namespace

// The friend seam declared in data_cloud_themes.h. It fills the catalog and,
// with it, marks the gift-theme list loaded: ChooseThemeController::fill()
// refuses to draw anything until myGiftThemesReady(), and that flag is only
// ever set by account.getUniqueGiftChatThemes - a request that never answers
// under the bridge, which is what kept the picker showing "No Theme" alone.
// Unique gifts have no FoxMes counterpart, so "loaded and empty" is the honest
// state, not a placeholder.
void Apply(
		not_null<Data::CloudThemes*> themes,
		std::vector<Data::CloudTheme> list) {
	themes->_chatThemes = std::move(list);
	themes->_myGiftThemesLoaded = true;
	// Fired on the next loop pass, not here. Upstream's catalog always lands
	// from a network reply, so callers subscribe to the update stream right
	// after asking for a refresh - CloudThemes::themeForTokenValue() does
	// exactly that. Filling and firing in one synchronous call would deliver
	// the event before that subscription exists, and the first chat opened
	// after launch would sit themeless until something else moved the stream.
	// Guarded on the session, which is what actually owns the lifetime here:
	// CloudThemes has no weak-pointer support of its own, and it dies with
	// Data::Session.
	crl::on_main(themes->_session, [themes] {
		themes->_chatThemesUpdates.fire({});
		themes->_myGiftThemesUpdates.fire({});
	});
}

void Request(not_null<Main::Session*> session) {
	const auto themes = &session->data().cloudThemes();
	if (!themes->chatThemes().empty()) {
		// Already built. Rebuilding would fire chatThemesUpdated again, and
		// the callers that ask for a refresh are subscribed to exactly that -
		// so every answer would ask another question.
		return;
	}
	Apply(themes, BuildCatalog(session));
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
		// A theme replaces the chat's own wallpaper, and upstream says so by
		// sending an empty messages.setChatWallPaper right before the theme
		// (SendPeerThemeChangeRequest). SetPeerTheme has already dropped the
		// paper locally; without the same clear on the server it would come
		// back on the next chat load.
		ClientFor(strong).setChatWallpaper(
			chatId,
			QString(),
			false,
			0,
			false,
			QString(),
			nullptr);
		ClientFor(strong).setChatTheme(chatId, emoticon, QString(), nullptr);
	});
}

void ApplyForPeer(not_null<PeerData*> peer, const QString &emoticon) {
	if (peer->themeToken() != emoticon) {
		peer->setThemeToken(emoticon);
	}
}

} // namespace CustomBackend::ChatThemes
