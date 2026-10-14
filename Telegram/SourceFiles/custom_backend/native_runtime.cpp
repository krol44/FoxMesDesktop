#include "custom_backend/native_runtime.h"
#include "custom_backend/native_appearance_adapter.h"

#include "custom_backend/native_gifs_adapter.h"
#include "custom_backend/native_reactions_adapter.h"
#include "custom_backend/native_peer_status_adapter.h"
#include "custom_backend/native_wallpaper_adapter.h"
#include "custom_backend/native_stickers_adapter.h"
#include "custom_backend/native_streaming_loader.h"
#include "custom_backend/native_topic_channels.h"

#include "base/algorithm.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_contacts_adapter.h"
#include "custom_backend/token_store.h"
#include "data/data_peer.h"
#include "data/data_user.h"
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

using details::Settings;

struct AuthContext {
    std::unique_ptr<ApiClient> client;
    QJsonObject user;
    base::weak_ptr<Main::Account> account;
    bool loaded = false;
};

std::unique_ptr<ApiClient> gLoginClient;
QJsonObject gLoginUser;
std::unordered_map<qint64, std::unique_ptr<AuthContext>> gContexts;
std::unordered_map<Main::Session*, std::unique_ptr<NativeBridge>> gBridges;
rpl::event_stream<Main::Session*> gBridgeChanges;

std::vector<std::function<void()>> gReleaseOnQuit;
bool gReleaseOnQuitConnected = false;

void ReleaseClientsOnQuit() {
    [[maybe_unused]] static const auto once = [] {
        ReleaseOnQuit([] {
            gContexts.clear();
            gLoginClient = nullptr;
        });
        return true;
    }();
}

QString EnvBaseUrl() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
    if (const auto env = std::getenv("FOXMES_URL")) {
        return QString::fromUtf8(env).trimmed();
    }
#endif
    return QString();
}

QString Prefix(qint64 userId) {
    return u"accounts/"_q + QString::number(userId);
}

void SaveEventSequence(qint64 userId, qint64 seq) {
	auto settings = Settings();
	settings.setValue(Prefix(userId) + u"/event_seq_v2"_q, seq);
	settings.sync();
}

void SaveContext(qint64 userId) {
    const auto i = gContexts.find(userId);
    if (i == gContexts.end() || !i->second || !i->second->client) return;
	const auto &context = *i->second;
	if (const auto account = context.account.get()) {
		SaveStoredAuth(
			account,
			context.client->accessToken(),
			context.user);
	}
	SaveEventSequence(userId, context.client->eventSequence());
}

AuthContext &EnsureContext(Main::Account *account, qint64 userId) {
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
    if (account && !context.account) {
        context.account = base::make_weak(account);
    }
    if (account && !context.loaded) {
        context.loaded = true;
        [[maybe_unused]] static const auto forgotten = [] {
            details::ForgetPlaintextData();
            return true;
        }();
        auto stored = LoadStoredAuth(account);
        context.user = std::move(stored.user);
        context.client->setTokens(std::move(stored.access));
        context.client->setMeId(userId);
        context.client->setEventSequence(Settings().value(
            Prefix(userId) + u"/event_seq_v2"_q).toLongLong());
    }
    return context;
}

qint64 SessionUserId(Main::Session *session) {
    return session ? qint64(session->userId().bare) : 0;
}

AuthContext &EnsureContext(Main::Session *session) {
    return EnsureContext(
        session ? &session->account() : nullptr,
        SessionUserId(session));
}

void ClearContext(Main::Account *account, qint64 userId) {
    if (userId <= 0) return;
    if (const auto i = gContexts.find(userId); i != gContexts.end()) {
        if (i->second && i->second->client) i->second->client->clearTokens();
        gContexts.erase(i);
    }
    if (account) {
        ClearStoredAuth(account);
        ClearStoredCaches(account);
    }
    auto settings = Settings();
    settings.remove(Prefix(userId));
    settings.sync();
}

} // namespace

bool Enabled() {
    return true;
}

TimeId AutoDeletePeriod(not_null<PeerData*> peer) {
    return (peer->isSelf() || TopicChannels::IsTopicChannelPeer(peer))
        ? TimeId(0)
        : kAutoDeletePeriod;
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
    return u"https://foxmes.foxtail.ing"_q;
#else
#if defined(_DEBUG)
    return u"http://0.0.0.0:7034"_q;
#else
    return u"https://foxmes.foxtail.ing"_q;
#endif
#endif
}

bool DevInsecureTls() {
    return !EnvBaseUrl().isEmpty();
}

DownloadAuth AuthorizeDownload(Main::Session *session, const QUrl &url) {
    if (!session || !url.isValid()) {
        return {};
    }
    const auto host = url.host().toLower();
    const auto api = QUrl(BaseUrl()).host().toLower();
    const auto ours = (host == u"cdn.foxtail.ing"_q)
        || (host == u"cdn.fxl.ru"_q)
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
    return *EnsureContext(session).client;
}

QJsonObject CurrentUser() {
    return gLoginUser;
}

QJsonObject CurrentUser(Main::Session *session) {
    return (SessionUserId(session) > 0)
        ? EnsureContext(session).user
        : QJsonObject();
}

bool HasStoredAuth(Main::Session *session) {
    if (!session) return false;
    auto &client = ClientFor(session);
    return !client.accessToken().isEmpty();
}

void RememberLogin(
        not_null<Main::Account*> account,
        const QJsonDocument &document) {
    if (!document.isObject()) return;
    const auto root = document.object();
    const auto user = root.value("user").toObject();
    const auto id = user.value("id").toVariant().toLongLong();
    if (id <= 0) return;

    gLoginUser = user;
    auto &context = EnsureContext(account, id);
    context.user = user;
    context.client->setTokens(Client().accessToken());
    context.client->setMeId(id);
    SaveContext(id);
}

void RememberUser(Main::Session *session, const QJsonObject &user) {
    const auto id = SessionUserId(session);
    if (id <= 0 || user.isEmpty()) return;
    auto &context = EnsureContext(session);
    auto incoming = user;
    if (!incoming.contains(u"event_seq"_q)) {
        for (const auto &key : { u"capabilities"_q, u"prompts"_q,
                u"can_create_group"_q, u"can_create_channel"_q,
                u"group_members_max"_q, u"channel_members_max"_q }) {
            if (!incoming.contains(key) && context.user.contains(key)) incoming.insert(key, context.user.value(key));
        }
    }
    incoming = PeerStatuses::Merge(session, session->user()->id, incoming);
    if (incoming.value(u"peer_appearance"_q).isObject()) {
        if (!incoming.contains(u"event_seq"_q)) {
            for (const auto &key : { u"capabilities"_q, u"prompts"_q,
                    u"can_create_group"_q, u"can_create_channel"_q,
                    u"group_members_max"_q, u"channel_members_max"_q }) {
                if (!incoming.contains(key) && context.user.contains(key)) {
                    incoming.insert(key, context.user.value(key));
                }
            }
        }
        const auto previous = context.user.value(u"peer_appearance"_q).toObject();
        if (previous.value(u"revision"_q).toVariant().toLongLong()
                > incoming.value(u"peer_appearance"_q).toObject().value(u"revision"_q).toVariant().toLongLong()) {
            incoming.insert(u"peer_appearance"_q, previous);
        }
    }
    context.user = incoming;
    context.client->setMeId(id);
    SaveContext(id);
}

void RememberEventSequence(Main::Session *session, qint64 seq) {
	const auto id = SessionUserId(session);
	if (id <= 0 || seq <= 0) return;
	EnsureContext(session).client->setEventSequence(seq);
	SaveEventSequence(id, seq);
}

QByteArray LoadChatsCache(Main::Session *session) {
	if (SessionUserId(session) <= 0) return {};
	return ReadStoredData(&session->account(), details::kChatsCachePref);
}

void SaveChatsCache(Main::Session *session, const QByteArray &json) {
	if (SessionUserId(session) <= 0 || json.isEmpty()) return;
	WriteStoredData(&session->account(), details::kChatsCachePref, json);
}

[[nodiscard]] std::string DefaultNotifyCacheKey(const QString &scope) {
	return details::NotifyDefaultsPref(scope.isEmpty()
		? std::string("user")
		: scope.toStdString());
}

QJsonObject LoadDefaultNotifyCache(
		Main::Session *session,
		const QString &scope) {
	if (SessionUserId(session) <= 0) return {};
	const auto document = QJsonDocument::fromJson(ReadStoredData(
		&session->account(),
		DefaultNotifyCacheKey(scope)));
	return document.isObject() ? document.object() : QJsonObject();
}

void SaveDefaultNotifyCache(
		Main::Session *session,
		const QJsonObject &settings) {
	if (SessionUserId(session) <= 0 || settings.isEmpty()) return;
	WriteStoredData(
		&session->account(),
		DefaultNotifyCacheKey(settings.value("scope").toString()),
		QJsonDocument(settings).toJson(QJsonDocument::Compact));
}

bool AppearanceDefaultApplied(Main::Session *session) {
	const auto id = SessionUserId(session);
	return (id <= 0)
		|| Settings().value(Prefix(id) + u"/appearance_default"_q).toBool();
}

void RememberAppearanceDefaultApplied(Main::Session *session) {
	const auto id = SessionUserId(session);
	if (id <= 0) return;
	auto settings = Settings();
	settings.setValue(Prefix(id) + u"/appearance_default"_q, true);
	settings.sync();
}

QJsonObject LoadReadJournal(Main::Session *session) {
	if (SessionUserId(session) <= 0) return {};
	const auto document = QJsonDocument::fromJson(ReadStoredData(
		&session->account(),
		details::kReadJournalPref));
	return document.isObject() ? document.object() : QJsonObject();
}

void SaveReadJournal(Main::Session *session, const QJsonObject &journal) {
	if (SessionUserId(session) <= 0) return;
	WriteStoredData(
		&session->account(),
		details::kReadJournalPref,
		journal.isEmpty()
			? QByteArray()
			: QJsonDocument(journal).toJson(QJsonDocument::Compact));
}

void ClearLogin(Main::Session *session) {
	if (const auto bridge = BridgeFor(session)) {
		bridge->stopLiveUpdates();
	}
    ClearContext(
        session ? &session->account() : nullptr,
        SessionUserId(session));
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
    const auto account = base::make_weak(&session->account());
    auto &client = ClientFor(session);
    if (client.accessToken().isEmpty()) {
        ClearContext(account.get(), id);
        if (done) done();
        return;
    }
    client.logout([=, done = std::move(done)](QJsonDocument, QString, int) mutable {
        ClearContext(account.get(), id);
        if (done) done();
    });
}

void AttachSession(Main::Session *session) {
    if (!session || !Enabled()) return;
    if (!HasStoredAuth(session)) {
        session->account().foxmesLoggedOut();
        return;
    }
    if (gBridges.find(session) == gBridges.end()) {
        gBridges.emplace(session, std::make_unique<NativeBridge>(session));
        Appearance::Load(session);
		gBridgeChanges.fire_copy(session);
    }
}

void DetachSession(Main::Session *session) {
    if (session) {
        Gifs::ClearSession(session);
        Stickers::ClearSession(session);
        Wallpapers::ClearSession(session);
        Streaming::ClearSession(session);
    }
    gBridges.erase(session);
	gBridgeChanges.fire_copy(session);
    if (session) {
        Reactions::ClearSession(session);
        PeerStatuses::ClearSession(session);
    }
}

NativeBridge *BridgeFor(Main::Session *session) {
    const auto i = gBridges.find(session);
    return (i == gBridges.end()) ? nullptr : i->second.get();
}

bool CanCreateGroups(Main::Session *session) {
    const auto bridge = session ? BridgeFor(session) : nullptr;
    return bridge && bridge->canCreateGroups();
}

bool CanCreateChannels(Main::Session *session) {
    const auto bridge = session ? BridgeFor(session) : nullptr;
    return bridge && bridge->canCreateChannels();
}

void UploadPeerPhoto(
        not_null<PeerData*> peer,
        QImage &&image,
        std::function<void()> done,
        std::function<void()> failed) {
    if (const auto user = peer->asUser(); user && user->isSelf()) {
        if (const auto bridge = BridgeFor(&peer->session())) bridge->uploadProfilePhoto(std::move(image), std::move(done), std::move(failed));
        return;
    }
    if (const auto user = peer->asUser(); user && !user->isSelf()) {
        Contacts::UploadPhoto(user, std::move(image), std::move(done));
        return;
    }
    const auto channel = peer->asChannel();
    const auto bridge = channel ? BridgeFor(&peer->session()) : nullptr;
    if (bridge) {
        bridge->uploadCommunityPhoto(channel, std::move(image), std::move(done));
    }
}

void UploadPeerVideoPhoto(
        not_null<PeerData*> peer,
        QImage &&cover,
        QByteArray &&video,
        double videoStartTs,
        std::function<void()> done,
        std::function<void()> fail) {
    const auto channel = peer->asChannel();
    const auto bridge = channel ? BridgeFor(&peer->session()) : nullptr;
    if (!bridge) {
        if (fail) {
            fail();
        }
        return;
    }
    bridge->uploadCommunityPhoto(
        channel,
        std::move(cover),
        std::move(done),
        std::move(video),
        videoStartTs,
        std::move(fail));
}

bool EphemeralMediaSupported(Main::Session *session) {
    const auto bridge = BridgeFor(session);
    return bridge && bridge->ephemeralMediaSupported();
}

void SaveNotifySettingsUpdates(
		Main::Session *session,
		base::flat_set<not_null<const Data::ForumTopic*>> topics,
		base::flat_set<not_null<const PeerData*>> peers,
		base::flat_set<Data::DefaultNotify> defaults) {
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

}

namespace CustomBackend {
void RequestProfilePhotos(not_null<UserData*> user, PhotoId afterId) {
    if (const auto bridge = BridgeFor(&user->session())) bridge->requestProfilePhotos(user, afterId);
}
void ChangeProfilePhoto(not_null<Main::Session*> session, PhotoId id, bool remove, Fn<void()> done) {
    if (const auto bridge = BridgeFor(session)) bridge->changeProfilePhoto(id, remove, std::move(done));
}
}
