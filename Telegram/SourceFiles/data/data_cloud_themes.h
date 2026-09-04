/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "data/data_wall_paper.h"

class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class Controller;
} // namespace Window

namespace Data {
class CloudThemes;
struct CloudTheme;
} // namespace Data

namespace CustomBackend::ChatThemes {
// FoxMes bridge seam: under the bridge the chat theme catalog is compiled into
// the client, not fetched with account.getChatThemes. Declared before
// Data::CloudThemes so the class can grant friendship; no FoxMes state lives
// in this header.
void Apply(
	not_null<Data::CloudThemes*> themes,
	std::vector<Data::CloudTheme> list);
} // namespace CustomBackend::ChatThemes

namespace Data {

struct UniqueGift;
class DocumentMedia;

enum class CloudThemeType {
	Dark,
	Light,
};

struct CloudTheme {
	uint64 id = 0;
	uint64 accessHash = 0;
	QString slug;
	QString title;
	DocumentId documentId = 0;
	UserId createdBy = 0;
	int usersCount = 0;
	QString emoticon;
	std::shared_ptr<UniqueGift> unique;

	using Type = CloudThemeType;
	struct Settings {
		std::optional<WallPaper> paper;
		QColor accentColor;
		std::optional<QColor> outgoingAccentColor;
		std::vector<QColor> outgoingMessagesColors;
	};
	base::flat_map<Type, Settings> settings;

	static CloudTheme Parse(
		not_null<Main::Session*> session,
		const MTPDtheme &data,
		bool parseSettings = false);
	static CloudTheme Parse(
		not_null<Main::Session*> session,
		const MTPDchatThemeUniqueGift &data,
		bool parseSettings = false);
	static CloudTheme Parse(
		not_null<Main::Session*> session,
		const MTPTheme &data,
		bool parseSettings = false);
};

class CloudThemes final {
public:
	explicit CloudThemes(not_null<Main::Session*> session);

	[[nodiscard]] static QString Format();

	void refresh();
	[[nodiscard]] rpl::producer<> updated() const;
	[[nodiscard]] const std::vector<CloudTheme> &list() const;
	void savedFromEditor(const CloudTheme &data);
	void remove(uint64 cloudThemeId);

	void refreshChatThemes();
	[[nodiscard]] const std::vector<CloudTheme> &chatThemes() const;
	[[nodiscard]] rpl::producer<> chatThemesUpdated() const;

	void myGiftThemesLoadMore(bool reload = false);
	[[nodiscard]] const std::vector<QString> &myGiftThemesTokens() const;
	[[nodiscard]] rpl::producer<> myGiftThemesUpdated() const;
	[[nodiscard]] QString processGiftThemeGetToken(
		const MTPDchatThemeUniqueGift &data);
	[[nodiscard]] bool myGiftThemesReady() const;

	void refreshChatThemesFor(const QString &token);
	[[nodiscard]] std::optional<CloudTheme> themeForToken(
		const QString &token) const;
	[[nodiscard]] auto themeForTokenValue(const QString &token)
		-> rpl::producer<std::optional<CloudTheme>>;

	[[nodiscard]] static bool TestingColors();
	static void SetTestingColors(bool testing);
	[[nodiscard]] QString prepareTestingLink(const CloudTheme &theme) const;
	[[nodiscard]] std::optional<CloudTheme> updateThemeFromLink(
		const QString &emoticon,
		const QMap<QString, QString> &params);

	void applyUpdate(const MTPTheme &theme);

	void resolve(
		not_null<Window::Controller*> controller,
		const QString &slug,
		const FullMsgId &clickFromMessageId);
	void showPreview(
		not_null<Window::Controller*> controller,
		const MTPTheme &data);
	void showPreview(
		not_null<Window::Controller*> controller,
		const CloudTheme &cloud);
	void applyFromDocument(const CloudTheme &cloud);

private:
	struct LoadingDocument {
		CloudTheme theme;
		DocumentData *document = nullptr;
		std::shared_ptr<Data::DocumentMedia> documentMedia;
		rpl::lifetime subscription;
		Fn<void(std::shared_ptr<Data::DocumentMedia>)> callback;
	};

	void parseThemes(const QVector<MTPTheme> &list);
	void checkCurrentTheme();
	void checkAppliedChatTheme();

	void install();
	void setupReload();
	[[nodiscard]] bool needReload() const;
	void scheduleReload();
	void reloadCurrent();
	void previewFromDocument(
		not_null<Window::Controller*> controller,
		const CloudTheme &cloud);
	void loadDocumentAndInvoke(
		LoadingDocument &value,
		const CloudTheme &cloud,
		not_null<DocumentData*> document,
		Fn<void(std::shared_ptr<Data::DocumentMedia>)> callback);
	void invokeForLoaded(LoadingDocument &value);

	void parseChatThemes(const QVector<MTPTheme> &list);

	const not_null<Main::Session*> _session;
	uint64 _hash = 0;
	mtpRequestId _refreshRequestId = 0;
	mtpRequestId _resolveRequestId = 0;
	std::vector<CloudTheme> _list;
	rpl::event_stream<> _updates;

	uint64 _chatThemesHash = 0;
	mtpRequestId _chatThemesRequestId = 0;
	std::vector<CloudTheme> _chatThemes;
	rpl::event_stream<> _chatThemesUpdates;

	// Minimal bridge seam: the FoxMes custom_backend fills the chat theme
	// catalog through this function; no state or business logic beyond it
	// lives here.
	// Qualified: the declarator names a function in another namespace, so GCC
	// looks the parameter types up in CustomBackend::ChatThemes, where the
	// sibling struct CloudTheme is not visible. CloudThemes resolves either way
	// as the injected-class-name, but spelling both out keeps the signature
	// identical to the declaration above.
	friend void CustomBackend::ChatThemes::Apply(
		not_null<Data::CloudThemes*> themes,
		std::vector<Data::CloudTheme> list);

	mtpRequestId _myGiftThemesRequestId = 0;
	base::flat_map<uint64, CloudTheme> _giftThemes;
	std::vector<QString> _myGiftThemesTokens;
	rpl::event_stream<> _myGiftThemesUpdates;
	uint64 _myGiftThemesHash = 0;
	QString _myGiftThemesNextOffset;
	bool _myGiftThemesLoaded = false;

	base::Timer _reloadCurrentTimer;
	LoadingDocument _updatingFrom;
	LoadingDocument _previewFrom;
	uint64 _installedDayThemeId = 0;
	uint64 _installedNightThemeId = 0;

	rpl::lifetime _lifetime;

};

} // namespace Data
