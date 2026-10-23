#include "custom_backend/api_client.h"

#include "base/debug_log.h"
#include "custom_backend/native_runtime.h"
#include "data/data_messages.h"

#include <QtCore/QSysInfo>

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QSettings>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

#include <algorithm>
#include <memory>
#include <utility>

namespace CustomBackend {
namespace {

QString normalizeBase(QUrl url) {
    auto text = url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    while (text.endsWith('/')) text.chop(1);
    return text;
}

QString errorFrom(const QByteArray &body, QNetworkReply *reply) {
    const auto parsed = QJsonDocument::fromJson(body);
    if (parsed.isObject()) {
        const auto message = parsed.object().value("error").toString();
        if (!message.isEmpty()) return message;
    }
    return reply->errorString();
}

constexpr auto kRequestTimeoutMs = 60'000;

constexpr auto kCallAttemptTimeoutMs = 10'000;
constexpr auto kCallResendWindowMs = 60'000;
constexpr auto kCallResendFirstDelayMs = 250;
constexpr auto kCallResendMaxDelayMs = 4'000;

constexpr auto kLaneHeader = "x-fxl-lane";

constexpr auto kUploadPath = "/upload/";
constexpr auto kUploadChunkPath = "/uploadChunk/";

void RememberLane(
        const std::shared_ptr<UploadLane> &lane,
        QNetworkReply *reply) {
    if (!lane) {
        return;
    }
    const auto value = reply->rawHeader(kLaneHeader).trimmed();
    if (!value.isEmpty()) {
        lane->value = value;
    }
}

QJsonArray idArray(const QList<qint64> &ids) {
    auto result = QJsonArray();
    for (const auto id : ids) result.append(id);
    return result;
}

QJsonArray stringsArray(const QStringList &values) {
    auto result = QJsonArray();
    for (const auto &value : values) result.append(value);
    return result;
}

QString UploadTypeFor(const QString &mime, bool forceFile, const QString &kind) {
    if (kind == u"voice"_q) {
        return u"audio-message"_q;
    } else if (kind == u"wallpaper"_q) {
        return u"wallpaper-chat"_q;
    } else if (kind == u"avatar"_q) {
        return u"avatar"_q;
    } else if (kind == u"contact_photo"_q) {
        return u"contact-photo"_q;
    } else if (forceFile) {
        return u"file"_q;
    } else if (mime == u"image/gif"_q) {
        return u"gif"_q;
    } else if (kind == u"document"_q) {
        return u"file"_q;
    } else if (kind == u"photo"_q) {
        return u"image"_q;
    } else if (kind == u"video"_q
        || kind == u"video_note"_q
        || kind == u"animation"_q) {
        return u"video"_q;
    } else if (kind == u"audio"_q) {
        return u"audio"_q;
    } else if (!kind.isEmpty()) {
        return u"file"_q;
    } else if (mime.startsWith(u"image/"_q)) {
        return u"image"_q;
    } else if (mime.startsWith(u"video/"_q)) {
        return u"video"_q;
    } else if (mime.startsWith(u"audio/"_q)) {
        return u"audio"_q;
    }
    return u"file"_q;
}

void ApplyDevTls(QNetworkRequest &request) {
    if (!DevInsecureTls()) {
        return;
    }
    auto configuration = request.sslConfiguration();
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(configuration);
}

struct MultipartPart {
    QByteArray disposition;
    QByteArray contentType;
    QByteArray body;
};

[[nodiscard]] QByteArray MultipartBoundary() {
    return "foxmes-" + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
}

[[nodiscard]] QByteArray MultipartBody(
        const std::vector<MultipartPart> &parts,
        const QByteArray &boundary) {
    auto result = QByteArray();
    for (const auto &part : parts) {
        result += "--" + boundary + "\r\n";
        result += "Content-Disposition: " + part.disposition + "\r\n";
        if (!part.contentType.isEmpty()) {
            result += "Content-Type: " + part.contentType + "\r\n";
        }
        result += "\r\n";
        result += part.body;
        result += "\r\n";
    }
    result += "--" + boundary + "--\r\n";
    return result;
}

[[nodiscard]] QByteArray MultipartType(const QByteArray &boundary) {
    return "multipart/form-data; boundary=" + boundary;
}

void AddUploadTarget(QUrlQuery &query, const UploadTarget &target) {
    if (target.chatId > 0) {
        query.addQueryItem(u"chatId"_q, QString::number(target.chatId));
    }
    if (target.topicId > 0) {
        query.addQueryItem(u"objectId"_q, QString::number(target.topicId));
    }
}

} // namespace

bool IsTransportFailure(int status) {
    return (status == 0) || (status == 502) || (status == 503) || (status == 504);
}

ApiClient::ApiClient(QUrl baseUrl, QObject *parent)
: QObject(parent)
, _baseUrl(normalizeBase(std::move(baseUrl))) {
    _sealed = std::make_shared<Sealed::Session>(
        &_network,
        _baseUrl,
        [](QNetworkRequest &request) { ApplyDevTls(request); });
    QObject::connect(
        &_network,
        &QNetworkAccessManager::finished,
        this,
        [=](QNetworkReply *reply) {
            const auto status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError
                && IsTransportFailure(status)) {
                _transportFailures.fire({});
            }
        });
    if (DevInsecureTls()) {
        QObject::connect(
            &_network,
            &QNetworkAccessManager::sslErrors,
            this,
            [](QNetworkReply *reply, const QList<QSslError> &) {
                reply->ignoreSslErrors();
            });
    }
}

Credentials ApiClient::credentials() const {
    const auto secret = _sealed->secret();
    return {
        .sid = _sealed->sid(),
        .secret = QByteArray(secret.data(), qsizetype(secret.size())),
    };
}

bool ApiClient::authorized() const {
    return _sealed->authorized();
}

std::weak_ptr<Sealed::Signer> ApiClient::signer() const {
    return _sealed->authorized()
        ? std::weak_ptr<Sealed::Signer>(_sealed)
        : std::weak_ptr<Sealed::Signer>();
}

void ApiClient::setCredentials(const Credentials &credentials) {
    if (credentials.empty()) {
        _sealed->clear();
        return;
    }
    _sealed->setCredentials(
        credentials.sid,
        Sealed::Bytes(credentials.secret.constData(), credentials.secret.size()));
    _sealed->prepare();
}

void ApiClient::clearTokens() {
    _sealed->clear();
    _login = nullptr;
    _meId = 0;
    if (_tokensChanged) _tokensChanged();
}

QNetworkReply *ApiClient::send(
        const QByteArray &method,
        const QNetworkRequest &request,
        const QByteArray &type,
        const QByteArray &body,
        bool authorize) {
    if (authorize) {
        return sendWith(_sealed, method, request, type, body);
    }
    auto plain = request;
    if (!type.isEmpty()) {
        plain.setHeader(QNetworkRequest::ContentTypeHeader, type);
    }
    if (method == "GET") {
        return _network.get(plain);
    } else if (method == "POST") {
        return _network.post(plain, body);
    } else if (method == "PUT") {
        return _network.put(plain, body);
    }
    return _network.sendCustomRequest(plain, method, body);
}

QNetworkReply *ApiClient::sendWith(
        std::weak_ptr<Sealed::Signer> signer,
        const QByteArray &method,
        const QNetworkRequest &request,
        const QByteArray &type,
        const QByteArray &body) {
    return new Sealed::Reply(&_network, Sealed::Spec{
        .method = method,
        .request = request,
        .type = type,
        .body = body,
    }, std::move(signer));
}

QNetworkRequest ApiClient::makeRequest(
        const QString &path,
        bool json,
        bool authorize,
        const LanePtr &lane,
        int timeoutMs) const {
    QNetworkRequest result(QUrl(normalizeBase(_baseUrl) + path));
    result.setTransferTimeout((timeoutMs > 0) ? timeoutMs : kRequestTimeoutMs);
    if (json) result.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    result.setRawHeader("Accept", "application/json");
    if (lane && !lane->value.isEmpty()) {
        result.setRawHeader(kLaneHeader, lane->value);
    }
    ApplyDevTls(result);
    return result;
}

void ApiClient::finish(
        QNetworkReply *reply,
        Callback done,
        const LanePtr &lane) {
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, lane, done = std::move(done)]() mutable {
        RememberLane(lane, reply);
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();
        auto document = QJsonDocument::fromJson(body);
        if (status == 429) {
            const auto retryAfter = reply->rawHeader("Retry-After").trimmed().toInt();
            if (retryAfter > 0) {
                auto object = document.isObject() ? document.object() : QJsonObject();
                object.insert("_retry_after_seconds", retryAfter);
                document = QJsonDocument(object);
            }
        }
        const auto error = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : errorFrom(body, reply);
        reply->deleteLater();
        if (done) done(document, error, status);
    });
}

void ApiClient::jsonRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonDocument &body,
        Callback done,
        bool authorize,
        const LanePtr &lane,
        int timeoutMs) {
	auto request = makeRequest(path, false, authorize, lane, timeoutMs);
    const auto payload = body.isNull() ? QByteArray() : body.toJson(QJsonDocument::Compact);
    if (method != "GET"
        && method != "POST"
        && method != "PATCH"
        && method != "PUT"
        && method != "DELETE") {
        if (done) done({}, u"unsupported HTTP method"_q, 0);
        return;
    }
    const auto type = payload.isEmpty()
        ? QByteArray()
        : QByteArray("application/json");
    const auto reply = send(method, request, type, payload, authorize);
    finish(reply, std::move(done), lane);
}

void ApiClient::markEphemeralViewed(qint64 messageId, Callback done) {
    jsonRequest(
        "POST",
        QString("/messages/%1/viewed").arg(messageId),
        {},
        std::move(done));
}

void ApiClient::acceptSession(const QJsonDocument &document) {
    if (!document.isObject()) return;
    const auto object = document.object();
    const auto sid = object.value("session_id").toVariant().toLongLong();
    const auto secret = QByteArray::fromBase64(
        object.value("session_secret").toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (sid > 0 && secret.size() == SealedCrypto::kKeySize) {
        _login = nullptr;
        setCredentials({ .sid = sid, .secret = secret });
    }
    const auto user = object.value("user").toObject();
    if (!user.isEmpty()) _meId = user.value("id").toVariant().toLongLong();
    if (_tokensChanged) _tokensChanged();
}

void ApiClient::startDevice(Callback done) {
    jsonRequest("GET", "/auth/login-key", {}, [this, done = std::move(done)](
            QJsonDocument doc,
            QString err,
            int status) mutable {
        if (!err.isEmpty() || status != 200) {
            if (done) done(std::move(doc), std::move(err), status);
            return;
        }
        startDeviceWithKey(doc, std::move(done));
    }, false);
}

void ApiClient::startDeviceWithKey(const QJsonDocument &keys, Callback done) {
    const auto key = Sealed::VerifyLoginDocument(
        keys.object(),
        QUrl(_baseUrl).host());
    const auto credential = key ? Sealed::StartLogin(*key) : std::nullopt;
    if (!credential) {
        LOG(("FoxMes Error: login key chain is not trusted."));
        if (done) done({}, u"login key is not trusted"_q, 0);
        return;
    }
    _login = std::make_shared<Sealed::FixedSigner>(*credential);
    auto device = QSysInfo::prettyProductName();
    if (device.isEmpty()) device = u"Unknown OS"_q;
    device += u" FoxMes Desktop"_q;
    const auto payload = QJsonDocument(QJsonObject{
        {"device_name", device},
    }).toJson(QJsonDocument::Compact);
    const auto reply = sendWith(
        _login,
        "POST",
        makeRequest(u"/auth/device/start"_q, false, false),
        "application/json",
        payload);
    finish(reply, std::move(done));
}

void ApiClient::pollDevice(const QString &request, Callback done) {
    if (!_login) {
        if (done) done({}, u"login is not started"_q, 0);
        return;
    }
    const auto payload = QJsonDocument(QJsonObject{
        {"request", request},
    }).toJson(QJsonDocument::Compact);
    const auto reply = sendWith(
        _login,
        "POST",
        makeRequest(u"/auth/device/exchange"_q, false, false),
        "application/json",
        payload);
    finish(reply, [this, done = std::move(done)](QJsonDocument doc, QString err, int status) mutable {
        if (err.isEmpty() && status == 200) acceptSession(doc);
        if (done) done(std::move(doc), std::move(err), status);
    });
}

void ApiClient::logout(Callback done) {
    jsonRequest("POST", "/auth/logout", QJsonDocument(QJsonObject{}), std::move(done));
}

void ApiClient::me(Callback done) {
    jsonRequest("GET", "/me", {}, [this, done = std::move(done)](QJsonDocument doc, QString err, int status) mutable {
        if (err.isEmpty() && doc.isObject()) {
            _meId = doc.object().value("id").toVariant().toLongLong();
        }
        if (done) done(std::move(doc), std::move(err), status);
    });
}

void ApiClient::desktopVersion(Callback done) {
    jsonRequest("GET", "/desktop/version", {}, std::move(done), false);
}

void ApiClient::updateMe(const QString &displayName, Callback done) {
    jsonRequest("PUT", "/me", QJsonDocument(QJsonObject{
        {"display_name", displayName},
    }), std::move(done));
}

void ApiClient::setUserBlocked(qint64 userId, bool blocked, Callback done) {
    jsonRequest(
        blocked ? "PUT" : "DELETE",
        QString("/users/%1/block").arg(userId),
        QJsonDocument(QJsonObject{
            {"operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        }),
        std::move(done));
}

void ApiClient::setUserContact(
        qint64 userId,
        QJsonObject fields,
        Callback done) {
    fields.insert(
        "operation_id",
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    jsonRequest(
        "PUT",
        QString("/users/%1/contact").arg(userId),
        QJsonDocument(fields),
        std::move(done));
}


void ApiClient::profilePhotos(qint64 userId, PhotoId afterId, Callback done) {
    jsonRequest("GET", QString("/users/%1/photos?max_id=%2&limit=100").arg(userId).arg(afterId), {}, std::move(done));
}

void ApiClient::changeProfilePhoto(PhotoId id, bool remove, qint64 revision, Callback done) {
    auto body = QJsonObject{
        {"operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"expected_revision", revision},
    };
    if (remove) body.insert("photo_ids", QJsonArray{QString::number(id)});
    jsonRequest(remove ? "DELETE" : "PUT", remove ? u"/me/photos"_q : QString("/me/photos/%1/main").arg(id), QJsonDocument(body), std::move(done));
}

void ApiClient::users(const QString &query, Callback done) {
    QUrlQuery q;
    if (!query.isEmpty()) q.addQueryItem("q", query);
    auto path = QString("/users");
    if (!q.isEmpty()) path += "?" + q.toString(QUrl::FullyEncoded);
    jsonRequest("GET", path, {}, std::move(done));
}

void ApiClient::user(qint64 userId, Callback done) {
    jsonRequest("GET", QString("/users/%1").arg(userId), {}, std::move(done));
}

void ApiClient::reactionsCatalog(Callback done) {
    jsonRequest("GET", "/reactions", {}, std::move(done));
}

void ApiClient::reactionUsage(Callback done) {
    jsonRequest("GET", "/reactions/usage", {}, std::move(done));
}

void ApiClient::chats(Callback done) {
    jsonRequest("GET", "/chats", {}, std::move(done));
}

void ApiClient::chatsLight(Callback done) {
    jsonRequest("GET", "/chats?light=1", {}, std::move(done));
}

void ApiClient::chat(qint64 chatId, Callback done) {
    jsonRequest("GET", QString("/chats/%1").arg(chatId), {}, std::move(done));
}

void ApiClient::savedChat(Callback done) {
    jsonRequest("GET", "/chats/saved", {}, std::move(done));
}

void ApiClient::directChat(qint64 userId, Callback done) {
    jsonRequest("POST", "/chats/direct", QJsonDocument(QJsonObject{
        {"user_id", userId},
    }), std::move(done));
}

void ApiClient::requestChat(qint64 userId, Callback done) {
    jsonRequest("POST", "/chats/direct/request", QJsonDocument(QJsonObject{
        {"user_id", userId},
        {"operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    }), std::move(done));
}

void ApiClient::answerChatRequest(qint64 chatId, bool accept, Callback done) {
    jsonRequest(
        "POST",
        QString("/chats/%1/request/%2"
            ).arg(chatId
            ).arg(accept ? u"accept"_q : u"decline"_q),
        QJsonDocument(QJsonObject{
            {"operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        }),
        std::move(done));
}

void ApiClient::forwardMessages(
        qint64 chatId,
        qint64 sourceChatId,
        qint64 threadRootId,
        const QList<qint64> &messageIds,
        bool dropAuthor,
        std::optional<int> videoTimestamp,
        const QString &operationId,
        Callback done) {
    auto body = QJsonObject{
        {"source_chat_id", sourceChatId},
        {"message_ids", idArray(messageIds)},
        {"operation_id", operationId},
        {"drop_author", dropAuthor},
    };
    if (videoTimestamp.has_value()) {
        body.insert("video_timestamp", *videoTimestamp);
    }
    if (threadRootId > 0) {
        body.insert("thread_root_id", threadRootId);
    }
    jsonRequest("POST", QString("/chats/%1/messages/forward").arg(chatId), QJsonDocument(body), std::move(done));
}

void ApiClient::pinMessage(
        qint64 chatId,
        qint64 messageId,
        bool forEveryone,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", QString("/chats/%1/pinned").arg(chatId), QJsonDocument(QJsonObject{
        {"message_id", messageId},
        {"for_everyone", forEveryone},
        {"operation_id", operationId},
    }), std::move(done));
}

void ApiClient::unpinMessage(
        qint64 chatId,
        qint64 messageId,
        const QString &operationId,
        Callback done) {
    jsonRequest("DELETE", QString("/chats/%1/pinned").arg(chatId), QJsonDocument(QJsonObject{
        {"message_id", messageId},
        {"operation_id", operationId},
    }), std::move(done));
}

void ApiClient::unpinAllMessages(
        qint64 chatId,
        const QString &operationId,
        Callback done) {
    jsonRequest("DELETE", QString("/chats/%1/pinned").arg(chatId), QJsonDocument(QJsonObject{
        {"all", true},
        {"operation_id", operationId},
    }), std::move(done));
}

void ApiClient::pinnedMessages(qint64 chatId, int limit, Callback done) {
    auto path = QString("/chats/%1/pinned").arg(chatId);
    if (limit > 0) {
        path += QString("?limit=%1").arg(limit);
    }
    jsonRequest("GET", path, {}, std::move(done));
}

void ApiClient::messages(qint64 chatId, qint64 aroundId, int limit, Data::LoadDirection direction, Callback done) {
    auto path = QString("/chats/%1/messages?limit=%2").arg(chatId).arg(limit);
    switch (direction) {
    case Data::LoadDirection::Before:
        if (aroundId > 0) path += QString("&before=%1").arg(aroundId);
        break;
    case Data::LoadDirection::After:
        if (aroundId > 0) path += QString("&after=%1").arg(aroundId);
        break;
    case Data::LoadDirection::Around:
        if (aroundId > 0) path += QString("&around=%1").arg(aroundId);
        break;
    }
    jsonRequest("GET", path, {}, std::move(done));
}

void ApiClient::message(qint64 chatId, qint64 messageId, Callback done) {
    jsonRequest("GET", QString("/chats/%1/messages/%2").arg(chatId).arg(messageId), {}, std::move(done));
}

void ApiClient::messageById(qint64 messageId, Callback done) {
	jsonRequest(
		"GET",
		QString("/messages/%1").arg(messageId),
		{},
		std::move(done));
}

void ApiClient::sendMessage(
        qint64 chatId,
        const QString &text,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool clearDraft,
        Callback done,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const QMap<qint64, AttachmentMeta> &meta) {
    sendMessageWithDraftRevision(
        chatId,
        text,
        replyToId,
        attachmentIds,
        clientNonce,
        clearDraft,
        0,
        std::move(done),
        forceFile,
        posters,
        meta);
}

namespace {

[[nodiscard]] QJsonObject PosterObject(const QMap<qint64, QString> &posters) {
    auto result = QJsonObject();
    for (auto i = posters.begin(); i != posters.end(); ++i) {
        if (!i.value().isEmpty()) {
            result.insert(QString::number(i.key()), i.value());
        }
    }
    return result;
}

[[nodiscard]] QJsonObject MetaEntry(const AttachmentMeta &meta) {
    auto entry = QJsonObject();
    if (!meta.kind.isEmpty()) entry.insert("kind", meta.kind);
    if (meta.durationMs > 0) entry.insert("duration_ms", meta.durationMs);
    if (!meta.waveform.isEmpty()) entry.insert("waveform", meta.waveform);
    if (!meta.performer.isEmpty()) entry.insert("performer", meta.performer);
    if (!meta.title.isEmpty()) entry.insert("title", meta.title);
    if (meta.spoiler) entry.insert("spoiler", true);
    return entry;
}

[[nodiscard]] QJsonObject MetaObject(
        const QMap<qint64, AttachmentMeta> &meta) {
    auto result = QJsonObject();
    for (auto i = meta.begin(); i != meta.end(); ++i) {
        const auto entry = MetaEntry(i.value());
        if (!entry.isEmpty()) {
            result.insert(QString::number(i.key()), entry);
        }
    }
    return result;
}

[[nodiscard]] QJsonArray AlbumItemArray(
        const QList<ApiClient::AlbumItem> &items) {
    auto result = QJsonArray();
    for (const auto &item : items) {
        auto object = QJsonObject{
            {"client_nonce", item.clientNonce},
            {"attachment_id", item.attachmentId},
        };
        const auto meta = MetaEntry(item.meta);
        if (!meta.isEmpty()) object.insert("meta", meta);
        result.append(object);
    }
    return result;
}

void ApplySendOptions(
        QJsonObject &body,
        const SendOptions &options) {
    if (options.silent) {
        body.insert("silent", true);
    }
    if (options.deliverWhenOnline) {
        body.insert("deliver_when_online", true);
    } else if (options.deliverAt != 0) {
        body.insert("deliver_at", options.deliverAt);
    }
}

void ApplyMediaTtl(QJsonObject &body, const SendOptions &options) {
    if (!options.ephemeral()) {
        return;
    }
    if (options.mediaTtlSeconds == kMediaTtlOnce) {
        body.insert("media_ttl_mode", "once");
        return;
    }
    body.insert("media_ttl_mode", "timer");
    body.insert("media_ttl_seconds", options.mediaTtlSeconds);
}

} // namespace

QJsonObject AttachmentMetaJson(const QMap<qint64, AttachmentMeta> &meta) {
    return MetaObject(meta);
}

QJsonObject AttachmentPostersJson(const QMap<qint64, QString> &posters) {
    return PosterObject(posters);
}

void ApiClient::sendMessageWithDraftRevision(
        qint64 chatId,
        const QString &text,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool clearDraft,
        qint64 clearDraftRevision,
        Callback done,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const QMap<qint64, AttachmentMeta> &meta,
        const QJsonArray &entities,
        const SendOptions &options) {
    auto body = QJsonObject{
        {"text", text},
        {"reply_to_id", replyToId},
        {"attachment_ids", idArray(attachmentIds)},
		{"client_nonce", clientNonce.isEmpty()
			? QUuid::createUuid().toString(QUuid::WithoutBraces)
			: clientNonce},
        {"clear_draft", clearDraft},
        {"force_file", forceFile},
        {"attachment_posters", PosterObject(posters)},
        {"attachment_meta", MetaObject(meta)},
    };
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    if (clearDraft && clearDraftRevision > 0) {
        body.insert("clear_draft_revision", clearDraftRevision);
    }
    if (options.silent) {
        body.insert("silent", true);
    }
    ApplyMediaTtl(body, options);
    jsonRequest("POST", QString("/chats/%1/messages").arg(chatId), QJsonDocument(body), std::move(done));
}

void ApiClient::sendMessageAlbum(
        qint64 chatId,
        const QString &caption,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool clearDraft,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const QMap<qint64, AttachmentMeta> &meta,
        Callback done) {
    sendMessage(
        chatId,
        caption,
        replyToId,
        attachmentIds,
        clientNonce,
        clearDraft,
        std::move(done),
        forceFile,
        posters,
        meta);
}
void ApiClient::sendAlbum(
        qint64 chatId,
        const QString &caption,
        const QJsonArray &entities,
        qint64 replyToId,
        const QList<AlbumItem> &items,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        Callback done,
        const SendOptions &options) {
    auto body = QJsonObject{
        {"text", caption},
        {"reply_to_id", replyToId},
        {"force_file", forceFile},
        {"items", AlbumItemArray(items)},
        {"attachment_posters", PosterObject(posters)},
    };
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    if (options.silent) {
        body.insert("silent", true);
    }
    ApplyMediaTtl(body, options);
    jsonRequest(
        "POST",
        QString("/chats/%1/messages").arg(chatId),
        QJsonDocument(body),
        std::move(done));
}

void ApiClient::reminders(qint64 chatId, Callback done) {
    jsonRequest(
        "GET",
        QString("/chats/%1/reminders").arg(chatId),
        {},
        std::move(done));
}

void ApiClient::createReminder(
        qint64 chatId,
        const QString &text,
        const QJsonArray &entities,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const QMap<qint64, AttachmentMeta> &meta,
        const SendOptions &options,
        const QString &operationId,
        Callback done) {
    auto body = QJsonObject{
        {"text", text},
        {"reply_to_id", replyToId},
        {"attachment_ids", idArray(attachmentIds)},
        {"client_nonce", clientNonce.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : clientNonce},
        {"force_file", forceFile},
        {"attachment_posters", PosterObject(posters)},
        {"attachment_meta", MetaObject(meta)},
        {"operation_id", operationId},
    };
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    ApplySendOptions(body, options);
    jsonRequest(
        "POST",
        QString("/chats/%1/reminders").arg(chatId),
        QJsonDocument(body),
        std::move(done));
}

void ApiClient::createReminderAlbum(
        qint64 chatId,
        const QString &caption,
        const QJsonArray &entities,
        qint64 replyToId,
        const QList<AlbumItem> &items,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const SendOptions &options,
        const QString &operationId,
        Callback done) {
    auto body = QJsonObject{
        {"text", caption},
        {"reply_to_id", replyToId},
        {"force_file", forceFile},
        {"items", AlbumItemArray(items)},
        {"attachment_posters", PosterObject(posters)},
        {"operation_id", operationId},
    };
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    ApplySendOptions(body, options);
    jsonRequest(
        "POST",
        QString("/chats/%1/reminders").arg(chatId),
        QJsonDocument(body),
        std::move(done));
}

void ApiClient::updateReminder(
        qint64 reminderId,
        const QString &text,
        const QJsonArray &entities,
        const SendOptions &options,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done) {
    auto body = QJsonObject{
        {"text", text},
        {"operation_id", operationId},
        {"expected_revision", expectedRevision},
    };
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    ApplySendOptions(body, options);
    jsonRequest(
        "PATCH",
        QString("/reminders/%1").arg(reminderId),
        QJsonDocument(body),
        std::move(done));
}

void ApiClient::deleteReminder(
        qint64 reminderId,
        bool all,
        const QString &operationId,
        Callback done) {
    auto path = QString("/reminders/%1?operation_id=%2")
        .arg(reminderId)
        .arg(operationId);
    if (all) {
        path += u"&all=1"_q;
    }
    jsonRequest("DELETE", path, {}, std::move(done));
}

void ApiClient::sendReminderNow(
        qint64 reminderId,
        const QString &operationId,
        Callback done) {
    jsonRequest(
        "POST",
        QString("/reminders/%1/send-now").arg(reminderId),
        QJsonDocument(QJsonObject{ {"operation_id", operationId} }),
        std::move(done));
}

void ApiClient::editMessage(
        qint64 messageId,
        const QString &text,
        const QJsonArray &entities,
        qint64 expectedRevision,
        Callback done) {
    auto body = QJsonObject{
        {"text", text},
        {"expected_revision", expectedRevision},
    };
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    jsonRequest("PATCH", QString("/messages/%1").arg(messageId), QJsonDocument(body), std::move(done));
}

void ApiClient::deleteMessage(qint64 messageId, Callback done) {
    jsonRequest("DELETE", QString("/messages/%1").arg(messageId), {}, std::move(done));
}

void ApiClient::deleteMessages(
        qint64 chatId,
        const QList<qint64> &messageIds,
        const QString &operationId,
        Callback done) {
    jsonRequest("DELETE", QString("/chats/%1/messages/batch-delete").arg(chatId), QJsonDocument(QJsonObject{
        {"message_ids", idArray(messageIds)},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::deleteHistory(qint64 chatId, Callback done) {
    jsonRequest("DELETE", QString("/chats/%1/history").arg(chatId), {}, std::move(done));
}

void ApiClient::deleteMessagesByDate(
        qint64 chatId,
        qint64 minDate,
        qint64 maxDate,
        Callback done) {
    jsonRequest("DELETE", QString("/chats/%1/messages/date-range").arg(chatId), QJsonDocument(QJsonObject{
        {"min_date", minDate},
        {"max_date", maxDate},
        {"revoke", true},
    }), std::move(done));
}

void ApiClient::deleteChat(qint64 chatId, Callback done) {
    jsonRequest("DELETE", QString("/chats/%1").arg(chatId), {}, std::move(done));
}

void ApiClient::setReactions(
        qint64 messageId,
        const std::vector<DocumentId> &reactions,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done) {
    auto ids = QJsonArray();
    for (const auto id : reactions) {
        ids.append(qint64(id));
    }
    jsonRequest("PUT", QString("/messages/%1/reactions").arg(messageId), QJsonDocument(QJsonObject{
        {"reactions", ids},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
        {"expected_revision", expectedRevision},
    }), std::move(done));
}

void ApiClient::operationResult(const QString &operationId, Callback done) {
    jsonRequest("GET", QString("/operations/%1").arg(operationId), {}, std::move(done));
}

void ApiClient::markRead(qint64 chatId, qint64 messageId, Callback done) {
    jsonRequest("POST", QString("/chats/%1/read").arg(chatId), QJsonDocument(QJsonObject{
        {"message_id", messageId},
    }), std::move(done));
}

void ApiClient::createMeet(qint64 chatId, const QString &operationId, Callback done) {
    jsonRequest("POST", QString("/chats/%1/meet").arg(chatId), QJsonDocument(QJsonObject{
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::callConfig(Callback done) {
    jsonRequest("GET", u"/calls/config"_q, QJsonDocument(), std::move(done));
}

void ApiClient::callDhConfig(Callback done) {
    jsonRequest("GET", u"/calls/dh-config"_q, QJsonDocument(), std::move(done));
}

void ApiClient::requestCall(
        qint64 chatId,
        const QByteArray &gaHash,
        bool video,
        const QJsonObject &protocol,
        const QString &operationId,
        Callback done) {
    jsonRequest("POST", u"/calls"_q, QJsonDocument(QJsonObject{
        {"chat_id", chatId},
        {"g_a_hash", QString::fromLatin1(gaHash.toBase64())},
        {"video", video},
        {"protocol", protocol},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

struct ApiClient::ResendingRequest {
    QByteArray method;
    QString path;
    QJsonDocument body;
    Callback done;
    QDeadlineTimer deadline;
    int attempt = 0;
};

void ApiClient::resendingJsonRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonDocument &body,
        Callback done) {
    sendResending(std::make_shared<ResendingRequest>(ResendingRequest{
        .method = method,
        .path = path,
        .body = body,
        .done = std::move(done),
        .deadline = QDeadlineTimer(kCallResendWindowMs),
    }));
}

void ApiClient::sendResending(std::shared_ptr<ResendingRequest> state) {
    jsonRequest(state->method, state->path, state->body, [=](QJsonDocument doc, QString error, int status) {
        if (!error.isEmpty()
            && IsTransportFailure(status)
            && !state->deadline.hasExpired()) {
            const auto delay = std::min(
                kCallResendFirstDelayMs << std::min(state->attempt, 4),
                kCallResendMaxDelayMs);
            ++state->attempt;
            LOG(("FoxMes API: %1 %2 transport failure '%3', resend #%4 in %5ms").arg(
                QString::fromLatin1(state->method),
                state->path,
                error,
                QString::number(state->attempt),
                QString::number(delay)));
            QTimer::singleShot(delay, this, [=] { sendResending(state); });
            return;
        }
        if (state->done) state->done(std::move(doc), std::move(error), status);
    }, true, nullptr, kCallAttemptTimeoutMs);
}

void ApiClient::callReceived(qint64 callId, Callback done) {
    resendingJsonRequest("POST", QString("/calls/%1/received").arg(callId), QJsonDocument(QJsonObject{}), std::move(done));
}

void ApiClient::callAccept(
        qint64 callId,
        const QByteArray &gb,
        const QJsonObject &protocol,
        Callback done) {
    resendingJsonRequest("POST", QString("/calls/%1/accept").arg(callId), QJsonDocument(QJsonObject{
        {"g_b", QString::fromLatin1(gb.toBase64())},
        {"protocol", protocol},
    }), std::move(done));
}

void ApiClient::callConfirm(
        qint64 callId,
        const QByteArray &ga,
        qint64 keyFingerprint,
        Callback done) {
    resendingJsonRequest("POST", QString("/calls/%1/confirm").arg(callId), QJsonDocument(QJsonObject{
        {"g_a", QString::fromLatin1(ga.toBase64())},
        {"key_fingerprint", keyFingerprint},
    }), std::move(done));
}

void ApiClient::callHeartbeat(qint64 callId, Callback done) {
    jsonRequest("POST", QString("/calls/%1/heartbeat").arg(callId), {}, std::move(done));
}

void ApiClient::callDiscard(
        qint64 callId,
        const QString &reason,
        int duration,
        const QString &slug,
        Callback done) {
    auto body = QJsonObject{
        {"reason", reason},
        {"duration", duration},
    };
    if (!slug.isEmpty()) {
        body.insert("slug", slug);
    }
    resendingJsonRequest("POST", QString("/calls/%1/discard").arg(callId), QJsonDocument(body), std::move(done));
}

void ApiClient::conferenceRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonObject &body,
        Callback done) {
    jsonRequest(
        method,
        path,
        (method == "GET") ? QJsonDocument() : QJsonDocument(body),
        std::move(done));
}

void ApiClient::communityRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonObject &body,
        Callback done) {
    jsonRequest(
        method,
        path,
        (method == "GET" || method == "DELETE")
            ? QJsonDocument()
            : QJsonDocument(body),
        std::move(done));
}

void ApiClient::hlsSegments(const QString &playlistSha256, Callback done) {
    jsonRequest(
        "GET",
        u"/files/hls/%1/segments"_q.arg(playlistSha256),
        QJsonDocument(),
        std::move(done));
}

void ApiClient::communityPhoto(
        qint64 chatId,
        const QByteArray &jpeg,
        const QByteArray &video,
        double videoStartTs,
        Callback done) {
    auto parts = std::vector<MultipartPart>{ {
        .disposition = R"(form-data; name="file"; filename="photo.jpg")",
        .contentType = "image/jpeg",
        .body = jpeg,
    } };
    if (!video.isEmpty()) {
        parts.push_back({
            .disposition = R"(form-data; name="video"; filename="photo.mp4")",
            .contentType = "video/mp4",
            .body = video,
        });
        parts.push_back({
            .disposition = R"(form-data; name="video_start_ts")",
            .body = QByteArray::number(videoStartTs, 'f', 3),
        });
    }
    const auto boundary = MultipartBoundary();
    const auto lane = std::make_shared<UploadLane>();
    auto request = makeRequest(
        QString("/chats/%1/photo").arg(chatId),
        false,
        true,
        lane);
    const auto reply = send(
        "PUT",
        request,
        MultipartType(boundary),
        MultipartBody(parts, boundary),
        true);
    finish(reply, std::move(done), lane);
}

void ApiClient::callSignaling(qint64 callId, const QByteArray &data, Callback done) {
    resendingJsonRequest("POST", QString("/calls/%1/signaling").arg(callId), QJsonDocument(QJsonObject{
        {"data", QString::fromLatin1(data.toBase64())},
    }), std::move(done));
}

void ApiClient::callSettings(Callback done) {
    jsonRequest("GET", u"/calls/settings"_q, QJsonDocument(), std::move(done));
}

void ApiClient::callSettingsUpdate(
        const QJsonObject &settings,
        Callback done) {
    jsonRequest("PUT", u"/calls/settings"_q, QJsonDocument(settings), std::move(done));
}

void ApiClient::callHistory(qint64 offsetId, int limit, Callback done) {
    auto path = QString("/calls/history?limit=%1").arg(limit);
    if (offsetId > 0) {
        path += QString("&offset_id=%1").arg(offsetId);
    }
    jsonRequest("GET", path, QJsonDocument(), std::move(done));
}

void ApiClient::callHistoryClear(Callback done) {
    jsonRequest("DELETE", u"/calls/history"_q, QJsonDocument(), std::move(done));
}

void ApiClient::markDelivered(
        qint64 chatId,
        const QList<qint64> &messageIds,
        Callback done) {
    jsonRequest("POST", QString("/chats/%1/delivered").arg(chatId), QJsonDocument(QJsonObject{
        {"message_ids", idArray(messageIds)},
    }), std::move(done));
}

void ApiClient::typing(qint64 chatId, Callback done) {
    jsonRequest("POST", QString("/chats/%1/typing").arg(chatId), QJsonDocument(QJsonObject{}), std::move(done));
}

void ApiClient::linkPreview(const QString &url, Callback done) {
    QUrlQuery q;
    q.addQueryItem("url", url);
    jsonRequest("GET", "/link-preview?" + q.toString(QUrl::FullyEncoded), {}, std::move(done));
}

void ApiClient::searchMessages(
        const QString &query,
        qint64 chatId,
        qint64 before,
        int limit,
        Callback done) {
    QUrlQuery q;
    q.addQueryItem("q", query);
    if (chatId > 0) q.addQueryItem("chat_id", QString::number(chatId));
    if (before > 0) q.addQueryItem("before", QString::number(before));
    if (limit > 0) q.addQueryItem("limit", QString::number(limit));
    jsonRequest("GET", "/search/messages?" + q.toString(QUrl::FullyEncoded), {}, std::move(done));
}

void ApiClient::globalMedia(
        const QString &kind,
        const QString &query,
        qint64 before,
        int limit,
        Callback done) {
    QUrlQuery q;
    q.addQueryItem("kind", kind);
    if (!query.isEmpty()) q.addQueryItem("q", query);
    if (before > 0) q.addQueryItem("before", QString::number(before));
    if (limit > 0) q.addQueryItem("limit", QString::number(limit));
    jsonRequest("GET", "/media?" + q.toString(QUrl::FullyEncoded), {}, std::move(done));
}

void ApiClient::chatMedia(
        qint64 chatId,
        const QString &kind,
        const QString &query,
        qint64 before,
        qint64 after,
        qint64 around,
        int limit,
        Callback done) {
    QUrlQuery q;
    q.addQueryItem("kind", kind);
    if (!query.isEmpty()) q.addQueryItem("q", query);
    if (before > 0) q.addQueryItem("before", QString::number(before));
    if (after > 0) q.addQueryItem("after", QString::number(after));
    if (around > 0) q.addQueryItem("around", QString::number(around));
    if (limit > 0) q.addQueryItem("limit", QString::number(limit));
    jsonRequest(
        "GET",
        QString("/chats/%1/media?").arg(chatId)
            + q.toString(QUrl::FullyEncoded),
        {},
        std::move(done));
}

void ApiClient::draft(qint64 chatId, Callback done) {
    jsonRequest("GET", QString("/chats/%1/draft").arg(chatId), {}, std::move(done));
}

void ApiClient::setDraft(
        qint64 chatId,
        const QString &text,
        qint64 replyToId,
        qint64 baseRevision,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", QString("/chats/%1/draft").arg(chatId), QJsonDocument(QJsonObject{
        {"text", text},
        {"reply_to_id", replyToId},
        {"base_revision", baseRevision},
        {"client_revision", 0},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::notificationSettings(qint64 chatId, Callback done) {
    jsonRequest("GET", QString("/chats/%1/notification-settings").arg(chatId), {}, std::move(done));
}

void ApiClient::setNotificationSettings(
        qint64 chatId,
        qint64 muteUntil,
        bool showPreviews,
        bool soundNone,
        Callback done) {
    jsonRequest("PUT", QString("/chats/%1/notification-settings").arg(chatId), QJsonDocument(QJsonObject{
        {"mute_until", muteUntil},
        {"show_previews", showPreviews},
        {"sound_none", soundNone},
    }), std::move(done));
}

void ApiClient::defaultNotificationSettings(
        const QString &scope,
        Callback done) {
    jsonRequest(
        "GET",
        u"/notification-settings/defaults?scope="_q + scope,
        {},
        std::move(done));
}

void ApiClient::setDefaultNotificationSettings(
        const QString &scope,
        qint64 muteUntil,
        bool soundNone,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", u"/notification-settings/defaults"_q, QJsonDocument(QJsonObject{
        {"scope", scope},
        {"mute_until", muteUntil},
        {"sound_none", soundNone},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::setChatPinned(qint64 chatId, bool pinned, Callback done) {
    jsonRequest("PUT", QString("/chats/%1/list-pin").arg(chatId), QJsonDocument(QJsonObject{
        {"pinned", pinned},
    }), std::move(done));
}

void ApiClient::setChatUnreadMark(qint64 chatId, bool markedUnread, Callback done) {
    jsonRequest("PUT", QString("/chats/%1/unread-mark").arg(chatId), QJsonDocument(QJsonObject{
        {"marked_unread", markedUnread},
    }), std::move(done));
}

void ApiClient::setChatArchived(qint64 chatId, bool archived, Callback done) {
    jsonRequest("PUT", QString("/chats/%1/archive").arg(chatId), QJsonDocument(QJsonObject{
        {"archived", archived},
    }), std::move(done));
}

void ApiClient::savePinnedOrder(const QList<qint64> &orderedIds, Callback done) {
    auto ids = QJsonArray();
    for (const auto id : orderedIds) {
        ids.append(qint64(id));
    }
    jsonRequest("PUT", QStringLiteral("/chats/list-pin-order"), QJsonDocument(QJsonObject{
        {"ordered_ids", ids},
    }), std::move(done));
}

void ApiClient::userThemes(Callback done) {
    jsonRequest("GET", u"/themes"_q, {}, std::move(done));
}
void ApiClient::saveUserTheme(qint64 id, QJsonObject body, Callback done) {
    body["operation_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    appearanceRequest(id ? "PUT" : "POST", id ? u"/themes/%1"_q.arg(id) : u"/themes"_q, std::move(body), std::move(done));
}
void ApiClient::deleteUserTheme(qint64 id, Callback done) {
    appearanceRequest("DELETE", u"/themes/%1"_q.arg(id), {}, std::move(done));
}
void ApiClient::setDefaultWallpaperState(QJsonObject body, Callback done) {
    body["operation_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    appearanceRequest("PUT", u"/wallpaper"_q, std::move(body), std::move(done));
}

void ApiClient::chatThemes(Callback done) {
    jsonRequest("GET", QStringLiteral("/chat-themes"), {}, std::move(done));
}

void ApiClient::wallpapers(Callback done) {
    jsonRequest("GET", QStringLiteral("/wallpapers"), {}, std::move(done));
}

void ApiClient::deleteWallpaper(const QString &sha256, Callback done) {
    jsonRequest("DELETE", u"/wallpapers/"_q + sha256, {}, std::move(done));
}

void ApiClient::defaultWallpaper(Callback done) {
    jsonRequest("GET", QStringLiteral("/wallpaper"), {}, std::move(done));
}

void ApiClient::setDefaultWallpaper(
        const QString &sha256,
        bool blurred,
        int intensity,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", QStringLiteral("/wallpaper"), QJsonDocument(QJsonObject{
        {"wallpaper_sha256", sha256},
        {"blurred", blurred},
        {"intensity", intensity},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::setChatWallpaper(
        qint64 chatId,
        const QString &sha256,
        bool blurred,
        int intensity,
        bool forBoth,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", QString("/chats/%1/wallpaper").arg(chatId), QJsonDocument(QJsonObject{
        {"wallpaper_sha256", sha256},
        {"blurred", blurred},
        {"intensity", intensity},
        {"for_both", forBoth},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::setChatNoWallpaper(
        qint64 chatId,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", QString("/chats/%1/wallpaper").arg(chatId), QJsonDocument(QJsonObject{
        {"wallpaper_sha256", QString()},
        {"blurred", false},
        {"intensity", 0},
        {"for_both", false},
        {"none", true},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::appearanceRequest(const QByteArray &method, const QString &path, QJsonObject body, Callback done) {
    const auto owner = meId();
    const auto base = baseUrl();
    const auto identity = base.toString() + u":"_q + QString::number(owner) + u":"_q + QString::fromLatin1(method) + path;
    const auto key = u"FoxMes/appearancePending/"_q + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    QSettings settings;
    auto previous = QJsonDocument::fromJson(settings.value(key).toByteArray()).object();
    const auto previousOperation = previous.take("operation_id").toString();
    body.remove("operation_id");
    body.insert("operation_id", !previousOperation.isEmpty() && previous == body ? previousOperation : QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto operation = body.value("operation_id").toString();
    settings.setValue(key, QJsonDocument(body).toJson(QJsonDocument::Compact));
    settings.sync();
    const auto weak = QPointer<ApiClient>(this);
    auto finish = [weak, owner, base, key, operation, done = std::move(done)](QJsonDocument doc, QString error, int status) mutable {
        if (!weak || weak->meId() != owner || weak->baseUrl() != base) return;
        if (error.isEmpty() || !IsTransportFailure(status)) {
            QSettings settings;
            if (QJsonDocument::fromJson(settings.value(key).toByteArray()).object().value("operation_id").toString() == operation) settings.remove(key);
        }
        if (done) done(std::move(doc), std::move(error), status);
    };
    jsonRequest(method, path, QJsonDocument(body),
        [weak, owner, base, operation, method, path, body, finish = std::move(finish)](QJsonDocument doc, QString error, int status) mutable {
            if (!weak || weak->meId() != owner || weak->baseUrl() != base) return;
            if (error.isEmpty() || !IsTransportFailure(status)) { finish(std::move(doc), std::move(error), status); return; }
            weak->operationResult(operation,
                [weak, owner, base, method, path, body, finish = std::move(finish)](QJsonDocument journal, QString error, int status) mutable {
                    if (!weak || weak->meId() != owner || weak->baseUrl() != base) return;
                    const auto object = journal.object();
                    if (error.isEmpty() && object.value("status").toString() == u"done"_q) finish(QJsonDocument(object.value("result").toObject()), {}, 200);
                    else if (error.isEmpty() && object.value("status").toString() == u"unknown"_q) weak->jsonRequest(method, path, QJsonDocument(body), std::move(finish));
                    else finish({}, error.isEmpty() ? u"Appearance confirmation unavailable"_q : error, 0);
                });
        });
}

void ApiClient::peerStatusRequest(const QByteArray &method, const QString &path, QJsonObject body, Callback done) {
    const auto identity = baseUrl().toString() + u":"_q + QString::number(meId()) + u":"_q + QString::fromLatin1(method) + path;
    const auto key = u"FoxMes/appearancePending/"_q + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    QSettings settings;
    auto previous = QJsonDocument::fromJson(settings.value(key).toByteArray()).object();
    previous.remove("operation_id");
    auto previousIntent = previous;
    previousIntent.remove("expected_revision");
    auto intent = body;
    intent.remove("expected_revision");
    intent.remove("operation_id");
    if (!previous.isEmpty() && previousIntent == intent) body = previous;
    appearanceRequest(method, path, std::move(body), std::move(done));
}

void ApiClient::setChatAppearance(qint64 chatId, QJsonObject body, Callback done) {
    appearanceRequest("PUT", QString("/chats/%1/appearance").arg(chatId), std::move(body), std::move(done));
}

void ApiClient::setChatTheme(
        qint64 chatId,
        const QString &emoticon,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", QString("/chats/%1/theme").arg(chatId), QJsonDocument(QJsonObject{
        {"theme_emoticon", emoticon},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::stickerFavorites(Callback done) {
    jsonRequest("GET", "/stickers/favorites", {}, std::move(done));
}

void ApiClient::setStickerFavorite(qint64 emojiId, bool favorited, const QString &operationId, Callback done) {
    jsonRequest("PUT", QString("/stickers/favorites/%1").arg(emojiId), QJsonDocument(QJsonObject{
        {"favorited", favorited}, {"operation_id", operationId},
    }), std::move(done));
}

void ApiClient::stickerSets(Callback done) {
    jsonRequest("GET", "/sticker-sets", {}, std::move(done));
}

void ApiClient::setStickerSetInstalled(qint64 setId, bool installed, const QString &operationId, Callback done) {
    jsonRequest("PUT", QString("/sticker-sets/%1/installed").arg(setId), QJsonDocument(QJsonObject{
        {"installed", installed}, {"operation_id", operationId},
    }), std::move(done));
}

void ApiClient::orderStickerSets(bool emoji, const QList<qint64> &setIds, const QString &operationId, Callback done) {
    jsonRequest("PUT", "/sticker-sets/order", QJsonDocument(QJsonObject{
        {"kind", emoji ? u"emoji"_q : u"sticker"_q},
        {"set_ids", idArray(setIds)},
        {"operation_id", operationId},
    }), std::move(done));
}

void ApiClient::emojiDocuments(const QList<qint64> &ids, Callback done) {
    auto list = QStringList();
    for (const auto id : ids) {
        list.push_back(QString::number(id));
    }
    jsonRequest("GET", u"/emoji-documents?ids="_q + list.join(','), {}, std::move(done));
}

void ApiClient::sendSticker(
        qint64 chatId,
        qint64 stickerId,
        qint64 replyToId,
        const QString &clientNonce,
        const SendOptions &options,
        Callback done) {
    auto body = QJsonObject{
        {"sticker_id", stickerId},
        {"reply_to_id", replyToId},
        {"client_nonce", clientNonce},
    };
    if (options.silent) {
        body.insert("silent", true);
    }
    jsonRequest("POST", QString("/chats/%1/messages").arg(chatId), QJsonDocument(body), std::move(done));
}

void ApiClient::createStickerReminder(
        qint64 chatId,
        qint64 stickerId,
        qint64 replyToId,
        const QString &clientNonce,
        const SendOptions &options,
        const QString &operationId,
        Callback done) {
    auto body = QJsonObject{
        {"sticker_id", stickerId},
        {"reply_to_id", replyToId},
        {"client_nonce", clientNonce},
        {"operation_id", operationId},
    };
    ApplySendOptions(body, options);
    jsonRequest("POST", QString("/chats/%1/reminders").arg(chatId), QJsonDocument(body), std::move(done));
}

void ApiClient::savedGifs(int limit, qint64 beforeId, Callback done) {
    auto path = QString("/gifs?limit=%1").arg(limit > 0 ? limit : 60);
    if (beforeId > 0) {
        path += u"&before_id="_q + QString::number(beforeId);
    }
    jsonRequest("GET", path, {}, std::move(done));
}

void ApiClient::saveGif(
        qint64 chatId,
        const QString &sha256,
        const QString &operationId,
        Callback done,
        qint64 messageId,
        qint64 threadRootId) {
    auto body = QJsonObject{
        {"chat_id", chatId},
        {"sha256", sha256},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    };
    if (threadRootId > 0) {
        body.insert("thread_root_id", threadRootId);
    } else if (messageId > 0) {
        body.insert("message_id", messageId);
    }
    jsonRequest("POST", QStringLiteral("/gifs"), QJsonDocument(body), std::move(done));
}

void ApiClient::deleteSavedGif(qint64 gifId, Callback done) {
    jsonRequest("DELETE", QString("/gifs/%1").arg(gifId), {}, std::move(done));
}

ApiClient::CancelHandle ApiClient::uploadDevice(
        const QString &name,
        QIODevice *device,
        const QString &mime,
        const UploadTarget &target,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    if (!device) {
        if (done) done({}, u"file device is null"_q, 0);
        return {};
    }
    const auto owned = std::unique_ptr<QIODevice>(device);
    if (!device->isOpen() && !device->open(QIODevice::ReadOnly)) {
        if (done) done({}, u"cannot read the upload source"_q, 0);
        return {};
    }
    auto filename = (name.isEmpty() ? u"upload.bin"_q : name);
    filename.replace('"', '_');
    filename.replace('\r', '_');
    filename.replace('\n', '_');
    auto parts = std::vector<MultipartPart>{ {
        .disposition = "form-data; name=\"file\"; filename=\""
            + filename.toUtf8()
            + "\"",
        .contentType = mime.toUtf8(),
        .body = device->readAll(),
    } };
    if (kind == u"avatar"_q) {
        parts.push_back({
            .disposition = R"(form-data; name="operation_id")",
            .body = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8(),
        });
    }
    const auto boundary = MultipartBoundary();


    QUrlQuery query;
    AddUploadTarget(query, target);
    const auto base = QString::fromLatin1(kUploadPath)
        + UploadTypeFor(mime, forceFile, kind);
    const auto path = query.isEmpty()
        ? base
        : base + u"?"_q + query.toString(QUrl::FullyEncoded);
    const auto lane = std::make_shared<UploadLane>();
    auto request = makeRequest(path, false, true, lane);
    const auto reply = send(
        "POST",
        request,
        MultipartType(boundary),
        MultipartBody(parts, boundary),
        true);
    if (progress) {
        QObject::connect(
            reply,
            &QNetworkReply::uploadProgress,
            this,
            [progress](qint64 sent, qint64 total) { progress(sent, total); });
    }
    finish(reply, std::move(done), lane);
    const auto guarded = QPointer<QNetworkReply>(reply);
    return [guarded] {
        if (guarded) guarded->abort();
    };
}

namespace {

constexpr auto kChunkMin = qint64(64 * 1024);
constexpr auto kChunkMax = qint64(32 * 1024 * 1024);
constexpr auto kChunkCountMax = qint64(1024);
constexpr auto kChunkPreferred = qint64(4 * 1024 * 1024);
constexpr auto kProcessingPollMs = 2000;
constexpr auto kChunkRetries = 3;
constexpr auto kChunkRetryBackoffMs = 600;
constexpr auto kChunkResyncs = 2;
constexpr auto kUploadRestarts = 1;
constexpr auto kCommitTimeoutMs = 5 * 60'000;

[[nodiscard]] qint64 ChunkSizeFor(qint64 size) {
    auto result = std::max(kChunkPreferred, (size + kChunkCountMax - 1) / kChunkCountMax);
    return std::clamp(result, kChunkMin, kChunkMax);
}

} // namespace

struct ApiClient::ChunkedUpload {
    std::unique_ptr<QIODevice> device;
    QString name;
    QString mime;
    QString type;
    QString uploadId;
    QString processId;
    qint64 size = 0;
    qint64 chunkSize = 0;
    qint64 sent = 0;
    int totalChunks = 0;
    int index = 0;
    int attempt = 0;
    int resyncs = 0;
    int restarts = 0;
    UploadTarget target;
    std::vector<bool> received;
    bool cancelled = false;
    bool finished = false;
    QPointer<QNetworkReply> reply;
    LanePtr lane = std::make_shared<UploadLane>();
    Callback done;
    ProgressCallback progress;

    [[nodiscard]] qint64 chunkBytesAt(int chunkIndex) const {
        const auto offset = qint64(chunkIndex) * chunkSize;
        return std::min(chunkSize, size - offset);
    }

    void recountSent() {
        sent = 0;
        for (auto i = 0; i != int(received.size()); ++i) {
            if (received[i]) {
                sent += chunkBytesAt(i);
            }
        }
    }

    void complete(QJsonDocument doc, QString error, int status) {
        if (finished) {
            return;
        }
        finished = true;
        if (done) done(std::move(doc), std::move(error), status);
    }
};

ApiClient::CancelHandle ApiClient::uploadPrepared(
        const QString &name,
        QIODevice *device,
        const QString &mime,
        const UploadTarget &target,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    const auto type = UploadTypeFor(mime, forceFile, kind);
    if (type == u"image"_q
        || type == u"avatar"_q
        || type == u"wallpaper-chat"_q
        || type == u"contact-photo"_q) {
        return uploadDevice(name, device, mime, target, forceFile, std::move(done), std::move(progress), kind);
    }
    return uploadDeviceChunked(name, device, mime, target, type, std::move(done), std::move(progress));
}

ApiClient::CancelHandle ApiClient::uploadDeviceChunked(
        const QString &name,
        QIODevice *device,
        const QString &mime,
        const UploadTarget &target,
        const QString &type,
        Callback done,
        ProgressCallback progress) {
    if (!device) {
        if (done) done({}, u"file device is null"_q, 0);
        return {};
    }
    const auto state = std::make_shared<ChunkedUpload>();
    state->device.reset(device);
    state->name = name.isEmpty() ? u"upload.bin"_q : name;
    state->mime = mime;
    state->type = type;
    state->size = device->size();
    state->done = std::move(done);
    state->progress = std::move(progress);
    if (state->size <= 0) {
        state->complete({}, u"empty upload"_q, 0);
        return {};
    }
    state->chunkSize = ChunkSizeFor(state->size);
    state->totalChunks = int((state->size + state->chunkSize - 1) / state->chunkSize);
    state->received.assign(state->totalChunks, false);

    state->target = target;
    startChunkSession(state);

    const auto weakSelf = QPointer<ApiClient>(this);
    return [weakSelf, state] {
        if (state->finished) {
            return;
        }
        state->cancelled = true;
        if (state->reply) {
            state->reply->abort();
        }
        if (!weakSelf
            || state->uploadId.isEmpty()
            || !state->processId.isEmpty()) {
            return;
        }
        auto query = QUrlQuery();
        query.addQueryItem(u"action"_q, u"abort"_q);
        query.addQueryItem(u"uploadId"_q, state->uploadId);
        weakSelf->jsonRequest(
            "POST",
            QString::fromLatin1(kUploadChunkPath)
                + state->type
                + u"?"_q
                + query.toString(QUrl::FullyEncoded),
            {},
            [](QJsonDocument, QString, int) {},
            true,
            state->lane);
    };
}

void ApiClient::startChunkSession(std::shared_ptr<ChunkedUpload> state) {
    const auto weak = QPointer<ApiClient>(this);
    auto query = QUrlQuery();
    query.addQueryItem(u"action"_q, u"init"_q);
    AddUploadTarget(query, state->target);
    const auto path = QString::fromLatin1(kUploadChunkPath)
        + state->type
        + u"?"_q
        + query.toString(QUrl::FullyEncoded);
    jsonRequest("POST", path, QJsonDocument(QJsonObject{
        {"name", state->name},
        {"size", state->size},
        {"mimeType", state->mime},
        {"totalChunks", state->totalChunks},
        {"chunkSize", state->chunkSize},
    }), [weak, state](QJsonDocument doc, QString error, int status) {
        if (!weak || state->cancelled) {
            state->complete({}, u"upload cancelled"_q, 0);
            return;
        }
        if (!error.isEmpty()) {
            state->complete({}, std::move(error), status);
            return;
        }
        state->uploadId = doc.object().value("uploadId").toString();
        if (state->uploadId.isEmpty()) {
            state->complete({}, u"upload init returned no id"_q, status);
            return;
        }
        weak->syncChunkedUpload(state, [weak, state] {
            if (weak) weak->sendNextChunk(state);
        });
    }, true, state->lane);
}

void ApiClient::syncChunkedUpload(
        std::shared_ptr<ChunkedUpload> state,
        Fn<void()> then) {
    auto query = QUrlQuery();
    query.addQueryItem(u"action"_q, u"status"_q);
    query.addQueryItem(u"uploadId"_q, state->uploadId);
    const auto path = QString::fromLatin1(kUploadChunkPath)
        + state->type
        + u"?"_q
        + query.toString(QUrl::FullyEncoded);
    const auto weak = QPointer<ApiClient>(this);
    jsonRequest("GET", path, {}, [weak, state, then = std::move(then)](
            QJsonDocument doc,
            QString error,
            int status) {
        if (state->cancelled) {
            state->complete({}, u"upload cancelled"_q, 0);
            return;
        }
        if (!error.isEmpty()
            && weak
            && weak->restartChunkedUpload(state, status)) {
            return;
        }
        if (error.isEmpty() && doc.isObject()) {
            state->received.assign(state->totalChunks, false);
            const auto list = doc.object().value("receivedChunks").toArray();
            for (const auto &value : list) {
                const auto index = value.toInt(-1);
                if (index >= 0 && index < state->totalChunks) {
                    state->received[index] = true;
                }
            }
            state->recountSent();
            if (state->progress) {
                state->progress(state->sent, state->size);
            }
        }
        state->index = 0;
        state->attempt = 0;
        if (then) then();
    }, true, state->lane);
}

void ApiClient::sendNextChunk(std::shared_ptr<ChunkedUpload> state) {
    if (state->cancelled) {
        state->complete({}, u"upload cancelled"_q, 0);
        return;
    }
    while (state->index < state->totalChunks
        && state->received[state->index]) {
        ++state->index;
    }
    if (state->index >= state->totalChunks) {
        finishChunkedUpload(std::move(state));
        return;
    }
    const auto offset = qint64(state->index) * state->chunkSize;
    const auto expected = std::min(state->chunkSize, state->size - offset);
    if (!state->device->seek(offset)) {
        state->complete({}, u"cannot seek the upload source"_q, 0);
        return;
    }
    const auto body = state->device->read(expected);
    if (body.size() != expected) {
        state->complete({}, u"short read from the upload source"_q, 0);
        return;
    }

    auto request = makeRequest(
        QString::fromLatin1(kUploadChunkPath) + state->type + u"?action=chunk"_q,
        false,
        true,
        state->lane);
    request.setRawHeader("X-Upload-ID", state->uploadId.toUtf8());
    request.setRawHeader("X-Chunk-Index", QByteArray::number(state->index));
    request.setRawHeader("X-Total-Chunks", QByteArray::number(state->totalChunks));
    const auto reply = send(
        "POST",
        request,
        "application/octet-stream",
        body,
        true);
    state->reply = reply;
    const auto weak = QPointer<ApiClient>(this);
    const auto chunkBytes = qint64(body.size());
    QObject::connect(reply, &QNetworkReply::finished, this, [weak, state, reply, chunkBytes] {
        RememberLane(state->lane, reply);
        const auto status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto payload = reply->readAll();
        const auto failed = (reply->error() != QNetworkReply::NoError);
        const auto message = failed ? errorFrom(payload, reply) : QString();
        reply->deleteLater();
        if (state->cancelled) {
            state->complete({}, u"upload cancelled"_q, 0);
            return;
        }
        if (failed) {
            if (weak) {
                weak->retryChunk(state, message, status);
            } else {
                state->complete({}, message, status);
            }
            return;
        }
        state->attempt = 0;
        state->sent += chunkBytes;
        state->received[state->index] = true;
        ++state->index;
        if (state->progress) {
            state->progress(state->sent, state->size);
        }
        if (weak) weak->sendNextChunk(state);
    });
    QObject::connect(reply, &QNetworkReply::uploadProgress, this, [state](qint64 sent, qint64) {
        if (state->progress && !state->cancelled) {
            state->progress(std::min(state->sent + sent, state->size), state->size);
        }
    });
}

bool ApiClient::restartChunkedUpload(
        std::shared_ptr<ChunkedUpload> state,
        int status) {
    if (status != 404 || state->restarts >= kUploadRestarts) {
        return false;
    }
    ++state->restarts;
    state->uploadId = QString();
    state->processId = QString();
    state->received.assign(state->totalChunks, false);
    state->index = 0;
    state->attempt = 0;
    state->resyncs = 0;
    state->sent = 0;
    state->lane = std::make_shared<UploadLane>();
    if (state->progress) {
        state->progress(0, state->size);
    }
    startChunkSession(state);
    return true;
}

void ApiClient::retryChunk(
        std::shared_ptr<ChunkedUpload> state,
        QString error,
        int status) {
    if (restartChunkedUpload(state, status)) {
        return;
    }
    if (state->attempt + 1 < kChunkRetries) {
        ++state->attempt;
        const auto delay = kChunkRetryBackoffMs * state->attempt;
        const auto weak = QPointer<ApiClient>(this);
        QTimer::singleShot(delay, this, [weak, state] {
            if (!weak || state->finished) {
                return;
            }
            if (state->cancelled) {
                state->complete({}, u"upload cancelled"_q, 0);
                return;
            }
            weak->sendNextChunk(state);
        });
        return;
    }
    if (state->resyncs >= kChunkResyncs) {
        state->complete({}, std::move(error), status);
        return;
    }
    ++state->resyncs;
    const auto weak = QPointer<ApiClient>(this);
    syncChunkedUpload(state, [weak, state] {
        if (weak) weak->sendNextChunk(state);
    });
}

void ApiClient::finishChunkedUpload(std::shared_ptr<ChunkedUpload> state) {
    const auto weak = QPointer<ApiClient>(this);
    jsonRequest("POST",
        QString::fromLatin1(kUploadChunkPath)
            + state->type
            + u"?action=complete"_q,
        QJsonDocument(QJsonObject{
            {"uploadId", state->uploadId},
            {"totalChunks", state->totalChunks},
            {"name", state->name},
            {"mimeType", state->mime},
        }),
        [weak, state](QJsonDocument doc, QString error, int status) {
            if (state->cancelled) {
                state->complete({}, u"upload cancelled"_q, 0);
                return;
            }
            if (error.isEmpty() && doc.isObject()) {
                const auto object = doc.object();
                const auto processId = object.value("processId").toString();
                if (object.value("processing").toBool() && !processId.isEmpty()) {
                    state->processId = processId;
                    if (weak) {
                        weak->pollChunkedProcessing(std::move(state));
                    } else {
                        state->complete({}, u"upload cancelled"_q, 0);
                    }
                    return;
                }
            }
            if (!error.isEmpty()
                && weak
                && weak->restartChunkedUpload(state, status)) {
                return;
            }
            state->complete(std::move(doc), std::move(error), status);
        }, true, state->lane, kCommitTimeoutMs);
}

void ApiClient::pollChunkedProcessing(std::shared_ptr<ChunkedUpload> state) {
    const auto weak = QPointer<ApiClient>(this);
    auto query = QUrlQuery();
    query.addQueryItem(u"action"_q, u"process"_q);
    query.addQueryItem(u"processId"_q, state->processId);
    const auto path = QString::fromLatin1(kUploadChunkPath)
        + state->type
        + u"?"_q
        + query.toString(QUrl::FullyEncoded);
    jsonRequest("POST", path, {}, [weak, state](
            QJsonDocument doc,
            QString error,
            int status) {
        if (state->cancelled) {
            state->complete({}, u"upload cancelled"_q, 0);
            return;
        }
        if (!error.isEmpty() || !doc.isObject()) {
            state->complete({}, std::move(error), status);
            return;
        }
        const auto object = doc.object();
        if (state->progress && !object.value("done").toBool()) {
            const auto percent = std::clamp(
                object.value("progress").toInt(),
                0,
                100);
            state->progress(state->size * percent / 100, state->size);
        }
        if (object.value("done").toBool()) {
            state->complete(std::move(doc), QString(), status);
            return;
        }
        if (!weak) {
            state->complete({}, u"upload cancelled"_q, 0);
            return;
        }
        QTimer::singleShot(
            kProcessingPollMs,
            weak.data(),
            [weak, state] {
                if (!weak || state->finished) {
                    return;
                }
                if (state->cancelled) {
                    state->complete({}, u"upload cancelled"_q, 0);
                    return;
                }
                weak->pollChunkedProcessing(state);
            });
    }, true, state->lane, kCommitTimeoutMs);
}

ApiClient::CancelHandle ApiClient::uploadFile(
        const QString &filePath,
        const QString &mime,
        qint64 chatId,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    return uploadFile(filePath, mime, UploadTarget{ .chatId = chatId }, forceFile, std::move(done), std::move(progress), kind);
}

ApiClient::CancelHandle ApiClient::uploadFile(
        const QString &filePath,
        const QString &mime,
        const UploadTarget &target,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind,
        const QString &displayName) {
    const auto effectiveMime = mime.isEmpty()
        ? QMimeDatabase().mimeTypeForFile(filePath).name()
        : mime;
    auto file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        if (done) done({}, file->errorString(), 0);
        delete file;
        return {};
    }
    return uploadPrepared(displayName.isEmpty() ? QFileInfo(filePath).fileName() : displayName, file, effectiveMime, target, forceFile, std::move(done), std::move(progress), kind);
}

ApiClient::CancelHandle ApiClient::uploadData(
        const QString &name,
        const QByteArray &data,
        const QString &mime,
        qint64 chatId,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    return uploadData(name, data, mime, UploadTarget{ .chatId = chatId }, forceFile, std::move(done), std::move(progress), kind);
}

ApiClient::CancelHandle ApiClient::uploadData(
        const QString &name,
        const QByteArray &data,
        const QString &mime,
        const UploadTarget &target,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    const auto effectiveMime = mime.isEmpty() ? u"application/octet-stream"_q : mime;
    auto buffer = new QBuffer();
    buffer->setData(data);
    if (!buffer->open(QIODevice::ReadOnly)) {
        if (done) done({}, u"failed to open upload buffer"_q, 0);
        delete buffer;
        return {};
    }
    return uploadPrepared(name, buffer, effectiveMime, target, forceFile, std::move(done), std::move(progress), kind);
}

void ApiClient::downloadFile(const QString &path, BytesCallback done) {
    const auto reply = send("GET", makeRequest(path, false), {}, {}, true);
    QObject::connect(reply, &QNetworkReply::finished, this,
        [reply, done = std::move(done)]() mutable {
            const auto status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto body = reply->readAll();
            const auto error = (reply->error() == QNetworkReply::NoError)
                ? QString()
                : errorFrom(body, reply);
            reply->deleteLater();
            if (done) done(body, error, status);
        });
}

void ApiClient::downloadUrl(const QUrl &url, BytesCallback done) {
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
        if (done) done({}, u"invalid download url"_q, 0);
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(kRequestTimeoutMs);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::ManualRedirectPolicy);
    ApplyDevTls(request);
    const auto reply = IsOurHost(url)
        ? send("GET", request, {}, {}, true)
        : _network.get(request);
    QObject::connect(reply, &QNetworkReply::finished, this,
        [reply, done = std::move(done)]() mutable {
            const auto status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto body = reply->readAll();
            QString error;
            if (reply->error() != QNetworkReply::NoError) {
                error = errorFrom(body, reply);
            } else if (status >= 300 && status < 400) {
                error = u"redirect is not allowed for downloads"_q;
            } else if (status != 200) {
                error = u"unexpected download status"_q;
            }
            reply->deleteLater();
            if (done) done(body, std::move(error), status);
        });
}

QUrl ApiClient::websocketUrl(qint64 since) const {
    auto url = _baseUrl;
    auto path = url.path();
    if (path.endsWith('/')) path.chop(1);
    url.setPath(path + u"/ws"_q);
    if (url.scheme() == u"https"_q) {
        url.setScheme(u"wss"_q);
    } else {
        url.setScheme(u"ws"_q);
    }
    QUrlQuery query;
    query.addQueryItem(u"since"_q, QString::number(since));
    url.setQuery(query);
    return url;
}

void ApiClient::websocketRequest(
		qint64 since,
		bool fresh,
		Fn<void(std::optional<Sealed::SocketRequest>)> done) {
	auto request = QNetworkRequest(websocketUrl(since));
	request.setTransferTimeout(kRequestTimeoutMs);
	request.setRawHeader("Accept", "application/json");
	ApplyDevTls(request);
	if (fresh) {
		_sealed->dropTemp();
	}
	const auto weak = QPointer<ApiClient>(this);
	_sealed->credential([=](std::optional<Sealed::Credential> credential) {
		if (!weak) {
			return;
		}
		done(credential
			? Sealed::SignSocket(request, *credential)
			: std::nullopt);
	});
}

}
