#include "custom_backend/native_runtime.h"

#include "custom_backend/native_gifs_adapter.h"
#include "custom_backend/native_wallpaper_adapter.h"
#include "custom_backend/native_stickers_adapter.h"
#include "custom_backend/native_streaming_loader.h"

#include "base/algorithm.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/token_store.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QUrl>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

#include "mtproto/facade.h"

#include <rpl/rpl.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

namespace CustomBackend {
namespace {

// The same store the tokens live in: it already carries the dev profile
// suffix, and the account data here has to follow the tokens.
using details::Settings;

struct AuthContext {
    std::unique_ptr<ApiClient> client;
    QJsonObject user;
    bool loaded = false;
};

std::unique_ptr<ApiClient> gLoginClient;
QJsonObject gLoginUser;
std::unordered_map<qint64, std::unique_ptr<AuthContext>> gContexts;
std::unordered_map<Main::Session*, std::unique_ptr<NativeBridge>> gBridges;
rpl::event_stream<Main::Session*> gBridgeChanges;

std::vector<std::function<void()>> gReleaseOnQuit;
bool gReleaseOnQuitConnected = false;

// The REST clients own a QNetworkAccessManager each, so they cannot outlive
// QCoreApplication - see ReleaseOnQuit() for what happens when they do.
void ReleaseClientsOnQuit() {
    [[maybe_unused]] static const auto once = [] {
        ReleaseOnQuit([] {
            gContexts.clear();
            gLoginClient = nullptr;
        });
        return true;
    }();
}

// Single source of truth for the FOXMES_URL override: it both selects the
// endpoint and marks the build as running against a dev environment.
QString EnvBaseUrl() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
    if (const auto env = std::getenv("FOXMES_URL")) {
        return QString::fromUtf8(env).trimmed();
    }
#endif // FOXMES_ALLOW_ENDPOINT_OVERRIDE
    return QString();
}

QString Prefix(qint64 userId) {
    return u"accounts/"_q + QString::number(userId);
}

void SaveContext(qint64 userId) {
    const auto i = gContexts.find(userId);
    if (i == gContexts.end() || !i->second || !i->second->client) return;
	auto settings = Settings();
	const auto prefix = Prefix(userId);
	SaveStoredTokens(userId, i->second->client->accessToken());
	settings.setValue(
		prefix + u"/user"_q,
		QJsonDocument(i->second->user).toJson(QJsonDocument::Compact));
    settings.setValue(prefix + u"/event_seq_v2"_q, i->second->client->eventSequence());
    settings.sync();
}

AuthContext &EnsureContext(qint64 userId) {
    ReleaseClientsOnQuit();
    auto &slot = gContexts[userId];
    if (!slot) slot = std::make_unique<AuthContext>();
    auto &context = *slot;
    if (!context.client) {
        context.client = std::make_unique<ApiClient>(QUrl(BaseUrl()));
        context.client->setMeId(userId);
        context.client->setTokensChangedCallback([userId] {
            SaveContext(userId);
        });
    }
    if (!context.loaded) {
        context.loaded = true;
        auto settings = Settings();
        const auto prefix = Prefix(userId);
        const auto stored = LoadStoredTokens(userId);
        auto access = stored.access;
        auto userBytes = settings.value(prefix + u"/user"_q).toByteArray();

        // One-time migration from Stage 3/4 global auth. Import it only into
        // the synthetic session whose user id actually matches the stored user.
        if (access.isEmpty()) {
            const auto legacyUserBytes = settings.value(u"auth/user"_q).toByteArray();
            const auto legacyDoc = QJsonDocument::fromJson(legacyUserBytes);
            const auto legacyUser = legacyDoc.isObject() ? legacyDoc.object() : QJsonObject();
            const auto legacyId = legacyUser.value("id").toVariant().toLongLong();
            if (legacyId == userId) {
                access = settings.value(u"auth/access"_q).toString();
                userBytes = legacyUserBytes;
                settings.remove(u"auth"_q);
            }
        }

        if (!userBytes.isEmpty()) {
            const auto doc = QJsonDocument::fromJson(userBytes);
            if (doc.isObject()) context.user = doc.object();
        }
        context.client->setTokens(access);
        context.client->setMeId(userId);
        // v2: the server switched from the global bigserial cursor to a
        // per-user continuous sequence, so the old stored cursor must be
        // ignored (one fresh replay, then normal delta sync).
        context.client->setEventSequence(settings.value(prefix + u"/event_seq_v2"_q).toLongLong());
        if (!access.isEmpty()) SaveContext(userId);
    }
    return context;
}

qint64 SessionUserId(Main::Session *session) {
    return session ? qint64(session->userId().bare) : 0;
}

void ClearContext(qint64 userId) {
    if (userId <= 0) return;
    if (const auto i = gContexts.find(userId); i != gContexts.end()) {
        if (i->second && i->second->client) i->second->client->clearTokens();
        gContexts.erase(i);
    }
    auto settings = Settings();
    ClearStoredTokens(userId);
    settings.remove(Prefix(userId));
    settings.sync();
}

} // namespace

bool Enabled() {
    return true;
}

TimeId AutoDeletePeriod(not_null<PeerData*> peer) {
    return peer->isSelf() ? TimeId(0) : kAutoDeletePeriod;
}

QString AutoDeleteBadgeText() {
    return tr::lng_days_tiny(tr::now, lt_count, kAutoDeletePeriod / 86400);
}

QString AutoDeleteInfoText() {
    return tr::lng_ttl_about_tooltip(
        tr::now,
        lt_duration,
        tr::lng_days(tr::now, lt_count, kAutoDeletePeriod / 86400));
}

QString BaseUrl() {
    if (const auto value = EnvBaseUrl(); !value.isEmpty()) {
        return value;
    }
#if !FOXMES_ALLOW_ENDPOINT_OVERRIDE
    return u"https://api-fox-mes.fxl.ru"_q;
#else
#if defined(_DEBUG)
    return u"http://0.0.0.0:7034"_q;
#else
    return u"https://api-fox-mes.fxl.ru"_q;
#endif
#endif // !FOXMES_ALLOW_ENDPOINT_OVERRIDE
}

bool DevInsecureTls() {
    return !EnvBaseUrl().isEmpty();
}

DownloadAuth AuthorizeDownload(Main::Session *session, const QUrl &url) {
    if (!session || !url.isValid()) {
        return {};
    }
    // Host allow-list, not a substring test: the bearer identifies the user,
    // so anything but our own CDN and our own API must come out of here empty.
    // The API is on the list because GET /link-preview/image is bearer'd like
    // every other route, and that one is read by the file loaders rather than
    // by the REST client. The dev host is only accepted in a dev build, where
    // FOXMES_URL is set.
    const auto host = url.host().toLower();
    const auto api = QUrl(BaseUrl()).host().toLower();
    const auto ours = (host == u"cdn.fxl.ru"_q)
        || (!api.isEmpty() && (host == api))
        || (DevInsecureTls() && (host == u"cdn.fxl.test"_q));
    if (!ours) {
        return {};
    }
    auto result = DownloadAuth{ .insecureTls = DevInsecureTls() };
    const auto token = ClientFor(session).accessToken();
    if (!token.isEmpty()) {
        result.authorization = "Bearer " + token.toUtf8();
    }
    return result;
}

void ApplyDownloadAuth(QNetworkRequest &request, const DownloadAuth &auth) {
    if (!auth.authorization.isEmpty()) {
        request.setRawHeader("Authorization", auth.authorization);
    }
    if (auth.insecureTls) {
        auto configuration = request.sslConfiguration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(configuration);
    }
}

bool AllowDownloadTls(QNetworkReply *reply, const DownloadAuth &auth) {
    if (!reply || !auth.insecureTls) {
        return false;
    }
    reply->ignoreSslErrors();
    return true;
}

void ReleaseOnQuit(std::function<void()> release) {
    if (!release) {
        return;
    }
    const auto app = QCoreApplication::instance();
    Assert(app != nullptr);
    if (!gReleaseOnQuitConnected) {
        gReleaseOnQuitConnected = true;
        // Runs after Sandbox::closeApplication(), which is connected first and
        // has already taken down Core::Application and every session by then.
        QObject::connect(app, &QCoreApplication::aboutToQuit, app, [] {
            for (const auto &release : base::take(gReleaseOnQuit)) {
                release();
            }
        });
    }
    gReleaseOnQuit.push_back(std::move(release));
}

ApiClient &Client() {
    ReleaseClientsOnQuit();
    if (!gLoginClient) {
        gLoginClient = std::make_unique<ApiClient>(QUrl(BaseUrl()));
    }
    return *gLoginClient;
}

ApiClient &ClientFor(Main::Session *session) {
    const auto id = SessionUserId(session);
    return *EnsureContext(id).client;
}

QJsonObject CurrentUser() {
    return gLoginUser;
}

QJsonObject CurrentUser(Main::Session *session) {
    const auto id = SessionUserId(session);
    return (id > 0) ? EnsureContext(id).user : QJsonObject();
}

bool HasStoredAuth(Main::Session *session) {
    if (!session) return false;
    auto &client = ClientFor(session);
    return !client.accessToken().isEmpty();
}

void RememberLogin(const QJsonDocument &document) {
    if (!document.isObject()) return;
    const auto root = document.object();
    const auto user = root.value("user").toObject();
    const auto id = user.value("id").toVariant().toLongLong();
    if (id <= 0) return;

    gLoginUser = user;
    auto &context = EnsureContext(id);
    context.user = user;
    context.client->setTokens(Client().accessToken());
    context.client->setMeId(id);
    SaveContext(id);
}

void RememberUser(Main::Session *session, const QJsonObject &user) {
    const auto id = SessionUserId(session);
    if (id <= 0 || user.isEmpty()) return;
    auto &context = EnsureContext(id);
    context.user = user;
    context.client->setMeId(id);
    SaveContext(id);
}

void RememberEventSequence(Main::Session *session, qint64 seq) {
	const auto id = SessionUserId(session);
	if (id <= 0 || seq <= 0) return;
	auto &context = EnsureContext(id);
	context.client->setEventSequence(seq);
	SaveContext(id);
}

QByteArray LoadChatsCache(Main::Session *session) {
	const auto id = SessionUserId(session);
	if (id <= 0) return {};
	return Settings().value(Prefix(id) + u"/chats_cache"_q).toByteArray();
}

void SaveChatsCache(Main::Session *session, const QByteArray &json) {
	const auto id = SessionUserId(session);
	if (id <= 0 || json.isEmpty()) return;
	auto settings = Settings();
	settings.setValue(Prefix(id) + u"/chats_cache"_q, json);
	settings.sync();
}

QJsonObject LoadDefaultNotifyCache(Main::Session *session) {
	const auto id = SessionUserId(session);
	if (id <= 0) return {};
	const auto raw = Settings().value(
		Prefix(id) + u"/notify_defaults"_q).toByteArray();
	const auto document = QJsonDocument::fromJson(raw);
	return document.isObject() ? document.object() : QJsonObject();
}

void SaveDefaultNotifyCache(
		Main::Session *session,
		const QJsonObject &settings) {
	const auto id = SessionUserId(session);
	if (id <= 0 || settings.isEmpty()) return;
	auto storage = Settings();
	storage.setValue(
		Prefix(id) + u"/notify_defaults"_q,
		QJsonDocument(settings).toJson(QJsonDocument::Compact));
	storage.sync();
}

QJsonObject LoadReadJournal(Main::Session *session) {
	const auto id = SessionUserId(session);
	if (id <= 0) return {};
	const auto raw = Settings().value(
		u"read_journal/accounts/"_q + QString::number(id)).toByteArray();
	const auto document = QJsonDocument::fromJson(raw);
	return document.isObject() ? document.object() : QJsonObject();
}

void SaveReadJournal(Main::Session *session, const QJsonObject &journal) {
	const auto id = SessionUserId(session);
	if (id <= 0) return;
	auto settings = Settings();
	const auto key = u"read_journal/accounts/"_q + QString::number(id);
	if (journal.isEmpty()) {
		settings.remove(key);
	} else {
		settings.setValue(key, QJsonDocument(journal).toJson(QJsonDocument::Compact));
	}
	settings.sync();
}

void ClearLogin(Main::Session *session) {
	if (const auto bridge = BridgeFor(session)) {
		bridge->stopLiveUpdates();
	}
    const auto id = SessionUserId(session);
    ClearContext(id);
}

void Logout(Main::Session *session, std::function<void()> done) {
    if (!session) {
        if (done) done();
        return;
    }
	if (const auto bridge = BridgeFor(session)) {
		bridge->stopLiveUpdates();
	}
    const auto id = SessionUserId(session);
    auto &client = ClientFor(session);
    if (client.accessToken().isEmpty()) {
        ClearContext(id);
        if (done) done();
        return;
    }
    client.logout([id, done = std::move(done)](QJsonDocument, QString, int) mutable {
        ClearContext(id);
        if (done) done();
    });
}

void AttachSession(Main::Session *session) {
    if (!session || !Enabled()) return;
    if (!HasStoredAuth(session)) {
        // Never borrow another account's REST token. Old Stage4 synthetic
        // sessions without their own token must authenticate independently.
        session->account().foxmesLoggedOut();
        return;
    }
    if (gBridges.find(session) == gBridges.end()) {
        gBridges.emplace(session, std::make_unique<NativeBridge>(session));
		gBridgeChanges.fire_copy(session);
    }
}

void DetachSession(Main::Session *session) {
    if (session) {
        // Both adapters hold DocumentData of this session; leaving them behind
        // would hand the next login dangling pointers.
        Gifs::ClearSession(session);
        Stickers::ClearSession(session);
        Wallpapers::ClearSession(session);
        Streaming::ClearSession(session);
    }
    gBridges.erase(session);
	gBridgeChanges.fire_copy(session);
}

NativeBridge *BridgeFor(Main::Session *session) {
    const auto i = gBridges.find(session);
    return (i == gBridges.end()) ? nullptr : i->second.get();
}

void SaveNotifySettingsUpdates(
		Main::Session *session,
		base::flat_set<not_null<const Data::ForumTopic*>> topics,
		base::flat_set<not_null<const PeerData*>> peers,
		base::flat_set<Data::DefaultNotify> defaults) {
	// Forum topics have no FoxMes counterpart: the product scope is private
	// chats and Saved Messages, and no UI under the bridge can queue one.
	// Named and ignored here rather than silently cleared in apiwrap.
	(void)topics;
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	for (const auto &peer : peers) {
		bridge->saveNotificationSettings(const_cast<PeerData*>(peer.get()));
	}
	for (const auto type : defaults) {
		bridge->saveDefaultNotifySettings(type);
	}
}

void TrackWindow(
		Main::Session *session,
		Window::SessionController *controller) {
	if (const auto bridge = BridgeFor(session)) {
		bridge->trackWindow(controller);
	}
}

LiveUpdatesStatus LiveUpdatesStatusFor(Main::Account *account) {
	if (account) {
		if (const auto session = account->maybeSession()) {
			if (const auto bridge = BridgeFor(session)) {
				return bridge->liveUpdatesStatus();
			}
		}
	}
	return {};
}

rpl::producer<LiveUpdatesStatus> LiveUpdatesStatusValue(
		Main::Account *account) {
	if (!account) {
		return rpl::single(LiveUpdatesStatus());
	}
	auto bridgeChanges = gBridgeChanges.events(
	) | rpl::filter([=](Main::Session *session) {
		return session && (&session->account() == account);
	});
	return rpl::merge(
		account->sessionValue(),
		std::move(bridgeChanges)
	) | rpl::map([](Main::Session *session)
			-> rpl::producer<LiveUpdatesStatus> {
		if (const auto bridge = BridgeFor(session)) {
			return bridge->liveUpdatesStatusValue();
		}
		return rpl::single(LiveUpdatesStatus());
	}) | rpl::flatten_latest(
	) | rpl::type_erased;
}

void RestartLiveUpdates(Main::Account *account) {
	if (account) {
		if (const auto session = account->maybeSession()) {
			if (const auto bridge = BridgeFor(session)) {
				bridge->restartLiveUpdates();
			}
		}
	}
}

int MtpDcStateFor(Main::Account *account) {
	constexpr auto kMinimalWaitingStateDuration = crl::time(4000);
	const auto status = LiveUpdatesStatusFor(account);
	switch (status.state) {
	case LiveUpdatesState::Connected:
		return MTP::ConnectedState;
	case LiveUpdatesState::Connecting:
		return MTP::ConnectingState;
	case LiveUpdatesState::Waiting: {
		// Negative value means milliseconds until retry, mirroring the
		// upstream dcstate() convention. Durations at or below the minimal
		// waiting threshold are reported as plain Connecting (see
		// window_connecting_widget.cpp for the same mapping).
		const auto remaining = std::max<crl::time>(
			1,
			status.retryAt - crl::now());
		return (remaining <= kMinimalWaitingStateDuration)
			? MTP::ConnectingState
			: -int(remaining);
	}
	}
	return MTP::ConnectingState;
}

} // namespace CustomBackend
