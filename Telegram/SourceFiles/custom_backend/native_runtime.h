#pragma once

#include "custom_backend/live_updates_connection.h"

#include <functional>

#include "base/flat_set.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QtGlobal>
#include <QtNetwork/QNetworkRequest>

class QNetworkReply;

class PeerData;
class QImage;
class HistoryItem;

namespace Data {
class ForumTopic;
enum class DefaultNotify : uint8_t;
}

namespace Main {
class Account;
class Session;
}

namespace Window {
class SessionController;
}

namespace CustomBackend {

class ApiClient;
class NativeBridge;

[[nodiscard]] bool Enabled();
inline constexpr bool DisableWhile = true;
inline constexpr bool HideGroupExtras = true;
[[nodiscard]] bool CanCreateGroups(Main::Session *session);
[[nodiscard]] bool CanCreateChannels(Main::Session *session);

inline constexpr auto kAutoDeletePeriod = TimeId(30 * 86400);

[[nodiscard]] TimeId AutoDeletePeriod(not_null<PeerData*> peer);
[[nodiscard]] QString AutoDeleteBadgeText();
[[nodiscard]] QString AutoDeleteInfoText();
[[nodiscard]] QString BaseUrl();
[[nodiscard]] bool DevInsecureTls();

struct DownloadAuth {
	QByteArray authorization;
	bool insecureTls = false;

	[[nodiscard]] bool empty() const {
		return authorization.isEmpty() && !insecureTls;
	}
};

[[nodiscard]] DownloadAuth AuthorizeDownload(
	Main::Session *session,
	const QUrl &url);
void ApplyDownloadAuth(QNetworkRequest &request, const DownloadAuth &auth);
bool AllowDownloadTls(QNetworkReply *reply, const DownloadAuth &auth);

void ReleaseOnQuit(std::function<void()> release);

[[nodiscard]] ApiClient &Client();
[[nodiscard]] ApiClient &ClientFor(Main::Session *session);
[[nodiscard]] QJsonObject CurrentUser();
[[nodiscard]] QJsonObject CurrentUser(Main::Session *session);
[[nodiscard]] bool HasStoredAuth(Main::Session *session);

void RememberLogin(const QJsonDocument &document);
void RememberUser(Main::Session *session, const QJsonObject &user);
void RememberEventSequence(Main::Session *session, qint64 seq);
[[nodiscard]] QByteArray LoadChatsCache(Main::Session *session);
void SaveChatsCache(Main::Session *session, const QByteArray &json);

[[nodiscard]] QJsonObject LoadDefaultNotifyCache(
	Main::Session *session,
	const QString &scope);
void SaveDefaultNotifyCache(Main::Session *session, const QJsonObject &settings);

[[nodiscard]] bool AppearanceDefaultApplied(Main::Session *session);
void RememberAppearanceDefaultApplied(Main::Session *session);
[[nodiscard]] QJsonObject LoadReadJournal(Main::Session *session);
void SaveReadJournal(Main::Session *session, const QJsonObject &journal);
void ClearLogin(Main::Session *session);
void Logout(Main::Session *session, std::function<void()> done = {});

void AttachSession(Main::Session *session);
void DetachSession(Main::Session *session);
[[nodiscard]] NativeBridge *BridgeFor(Main::Session *session);

void MarkEphemeralViewed(
	const base::flat_set<not_null<HistoryItem*>> &items);

[[nodiscard]] bool EphemeralMediaSupported(Main::Session *session);
void UploadPeerPhoto(
	not_null<PeerData*> peer,
	QImage &&image,
	std::function<void()> done);
void UploadPeerVideoPhoto(
	not_null<PeerData*> peer,
	QImage &&cover,
	QByteArray &&video,
	double videoStartTs,
	std::function<void()> done,
	std::function<void()> fail);

void AttachEphemeralMediaUrl(
	Main::Session *session,
	HistoryItem *item,
	const QString &url);

void SaveNotifySettingsUpdates(
	Main::Session *session,
	base::flat_set<not_null<const Data::ForumTopic*>> topics,
	base::flat_set<not_null<const PeerData*>> peers,
	base::flat_set<Data::DefaultNotify> defaults);
void TrackWindow(
	Main::Session *session,
	Window::SessionController *controller);
[[nodiscard]] LiveUpdatesStatus LiveUpdatesStatusFor(Main::Account *account);
[[nodiscard]] rpl::producer<LiveUpdatesStatus> LiveUpdatesStatusValue(
	Main::Account *account);
void RestartLiveUpdates(Main::Account *account);
[[nodiscard]] int MtpDcStateFor(Main::Account *account);

}
