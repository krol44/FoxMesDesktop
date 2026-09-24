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
// Group and channel features FoxMes does not have: topics, reaction settings,
// permissions, invite links, administrators, polls, boosts and the rest
// listed in FOXMES_BRIDGE.md ("Правила групп и каналов"). Upstream code stays;
// only its entry points are hidden by this.
inline constexpr bool HideGroupExtras = true;
// "New Group" / "New Channel" are shown only to accounts holding the
// foxMes.createGroup / foxMes.createChannel access rule.
[[nodiscard]] bool CanCreateGroups(Main::Session *session);
[[nodiscard]] bool CanCreateChannels(Main::Session *session);

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
[[nodiscard]] ApiClient &ClientFor(Main::Session *session);
[[nodiscard]] QJsonObject CurrentUser();
[[nodiscard]] QJsonObject CurrentUser(Main::Session *session);
[[nodiscard]] bool HasStoredAuth(Main::Session *session);

void RememberLogin(const QJsonDocument &document);
void RememberUser(Main::Session *session, const QJsonObject &user);
void RememberEventSequence(Main::Session *session, qint64 seq);
[[nodiscard]] QByteArray LoadChatsCache(Main::Session *session);
void SaveChatsCache(Main::Session *session, const QByteArray &json);

// Last server-accepted notification defaults. Seeded into the native model
// before the first response arrives: an unknown default makes every unmuted
// peer read as muted, so the cache is what keeps the setting from flickering
// across a restart.
// One per scope ("user", "group", "channel"); settings carry their scope.
[[nodiscard]] QJsonObject LoadDefaultNotifyCache(
	Main::Session *session,
	const QString &scope);
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

// The bridge's messages.readMessageContents: one call, sent when the content
// was actually shown, which both spends a "view once" and starts the countdown.
void MarkEphemeralViewed(
	const base::flat_set<not_null<HistoryItem*>> &items);

// Disappearing media, answered by the server through GET /me capabilities.
// False until it has answered: a picker shown against a server that would
// refuse the send is worse than no picker at all. The other direction - an old
// build against a new server - is not held by this flag and cannot be: it is
// held by the message contract, which never puts the attachment of an
// ephemeral message into any projection. Per account, since each one asks
// its own server session.
[[nodiscard]] bool EphemeralMediaSupported(Main::Session *session);
// The photo of a group or channel. Upstream's uploader sends MTProto file
// parts, which go nowhere under the bridge, so Api::PeerPhoto hands the image
// here and it goes to PUT /chats/{id}/photo. done runs on success only, like
// upstream's.
void UploadPeerPhoto(
	not_null<PeerData*> peer,
	QImage &&image,
	std::function<void()> done);

// Points the media of a disappearing message at the url a view session just
// granted. It lives beside the bridge because the photo path needs
// UpdateRemotePhotoImages, which owns the size ladder fxl-cdn serves and has
// no business being copied into an adapter.
void AttachEphemeralMediaUrl(
	Main::Session *session,
	HistoryItem *item,
	const QString &url);

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
[[nodiscard]] int MtpDcStateFor(Main::Account *account);

} // namespace CustomBackend
