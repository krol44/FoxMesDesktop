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

// Mirrors ChatMessageRetention in fxl-cron (internal/jobs/chats.go): the
// backend drops chat messages older than this and keeps the personal chat and
// the pinned ones. Display only - the client never deletes on its own, the
// removal arrives as a message.deleted event.
inline constexpr auto kAutoDeletePeriod = TimeId(30 * 86400);

// Zero for the personal chat, which the backend never cleans up.
[[nodiscard]] TimeId AutoDeletePeriod(not_null<PeerData*> peer);
// Upstream formatters round this period to "1m" and "4 weeks 2 days", while
// the product promises exactly 30 days.
[[nodiscard]] QString AutoDeleteBadgeText();
[[nodiscard]] QString AutoDeleteInfoText();
[[nodiscard]] QString BaseUrl();
// Dev profile: the endpoint was substituted through FOXMES_URL
// (dev-client.sh). Local fxl-api and fxl-cdn serve certificates that do not
// match the host, so peer verification is turned off in that mode. A normal
// build has no FOXMES_URL, returns false here and verifies TLS as before.
[[nodiscard]] bool DevInsecureTls();

// What a download of a FoxMes attachment needs on top of a plain GET.
//
// Attachments live on fxl-cdn, which requires the session bearer, so upstream
// loaders have to carry it. It is deliberately scoped to our own CDN host:
// the token must never be attached to a request to any other origin, and an
// empty value here means "send nothing extra".
struct DownloadAuth {
	QByteArray authorization;
	bool insecureTls = false;

	[[nodiscard]] bool empty() const {
		return authorization.isEmpty() && !insecureTls;
	}
};

// Resolve on the thread that owns the session; loaders may run elsewhere.
[[nodiscard]] DownloadAuth AuthorizeDownload(
	Main::Session *session,
	const QUrl &url);
void ApplyDownloadAuth(QNetworkRequest &request, const DownloadAuth &auth);
// Tolerate the reply's tls errors, and report whether they were tolerated.
//
// Needed twice over: WebLoadManager treats any sslErrors as a hard failure,
// and Qt emits that signal even for a reply that already ignores them - so the
// answer also has to gate upstream's own handler. False everywhere outside a
// dev build, where certificates are verified as usual.
bool AllowDownloadTls(QNetworkReply *reply, const DownloadAuth &auth);

// Releases a network owner while QCoreApplication is still alive.
//
// QNetworkAccessManager runs its own worker thread and joins it from its
// destructor through the Qt event system. One that is still alive when the
// static destructors run - after ~QCoreApplication - waits on a thread nothing
// can tell to quit any more, so exit() never returns and the process hangs
// with its dock icon left behind. Anything that keeps a manager past the end
// of a scope registers its teardown here, and it runs on aboutToQuit instead.
void ReleaseOnQuit(std::function<void()> release);

// Client() is the unaffiliated login/register client used by Intro.
[[nodiscard]] ApiClient &Client();
// Every authenticated Main::Session has its own REST token context.
[[nodiscard]] ApiClient &ClientFor(Main::Session *session);
[[nodiscard]] QJsonObject CurrentUser();
[[nodiscard]] QJsonObject CurrentUser(Main::Session *session);
[[nodiscard]] bool HasStoredAuth(Main::Session *session);

void RememberLogin(const QJsonDocument &document);
void RememberUser(Main::Session *session, const QJsonObject &user);
void RememberEventSequence(Main::Session *session, qint64 seq);
// Light chat list snapshot ("GET /chats?light=1" JSON) for instant render
// before the network answers on cold start.
[[nodiscard]] QByteArray LoadChatsCache(Main::Session *session);
void SaveChatsCache(Main::Session *session, const QByteArray &json);

// Last server-accepted notification defaults. Seeded into the native model
// before the first response arrives: an unknown default makes every unmuted
// peer read as muted, so the cache is what keeps the setting from flickering
// across a restart.
[[nodiscard]] QJsonObject LoadDefaultNotifyCache(Main::Session *session);
void SaveDefaultNotifyCache(Main::Session *session, const QJsonObject &settings);

// Whether this account has already been given the first-run look the server
// describes (GET /wallpaper -> "appearance"). Remembered per account and per
// install, because the colour theme is a setting of this install that the
// server never hears about: without the mark, every start would undo a theme
// the user picked afterwards.
[[nodiscard]] bool AppearanceDefaultApplied(Main::Session *session);
void RememberAppearanceDefaultApplied(Main::Session *session);
// Durable read intent is deliberately stored outside the auth context. A 401
// clears tokens and pauses the journal, but the intent must survive re-auth.
[[nodiscard]] QJsonObject LoadReadJournal(Main::Session *session);
void SaveReadJournal(Main::Session *session, const QJsonObject &journal);
void ClearLogin(Main::Session *session);
void Logout(Main::Session *session, std::function<void()> done = {});

void AttachSession(Main::Session *session);
void DetachSession(Main::Session *session);
[[nodiscard]] NativeBridge *BridgeFor(Main::Session *session);

// Drains the upstream delayed notify-settings queue. Takes the whole queue so
// the decision of what the bridge can persist lives here and not in apiwrap:
// topics have no FoxMes counterpart (forums are out of scope) and are dropped
// deliberately, peers and per-type defaults are saved over REST.
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
// Projects the live updates WebSocket state onto upstream MTP dcstate
// semantics (mtproto/facade.h), so thin upstream hooks like
// TopBarWidget::updateConnectingState() can reuse their original logic.
[[nodiscard]] int MtpDcStateFor(Main::Account *account);

} // namespace CustomBackend
