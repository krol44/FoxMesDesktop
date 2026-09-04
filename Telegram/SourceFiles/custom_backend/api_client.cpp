#include "custom_backend/api_client.h"

#include "custom_backend/native_runtime.h"
#include "data/data_messages.h"

#include <QtCore/QSysInfo>

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtNetwork/QHttpMultiPart>
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

// The name Envoy is configured with, in both the api and the FoxMes route
// tables. Lower case on purpose: it is compared case-insensitively by Qt, and
// this is how the config and fxl-web spell it.
constexpr auto kRequestTimeoutMs = 60'000;

constexpr auto kLaneHeader = "x-fxl-lane";

// Chat uploads have their own routes on both APIs: that is where video is
// stored as it arrives instead of being sliced into HLS or re-encoded
// (fxl-api ChatUploadOptions).
constexpr auto kUploadPath = "/chat/upload/";
constexpr auto kUploadChunkPath = "/chat/uploadChunk/";

// Pinning is a routing hint, not part of the upload protocol, so a missing
// header never fails a request: the route may simply not be pinned. An empty
// answer also must not clear what is already held - the chain is bound to that
// pod for the rest of its life.
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

// The canonical fxl-api upload is typed, and the type is not cosmetic: only
// image (and emoji/avatar/video-poster) go through the sanitizer that fills
// files_real.width/height, which the bubble needs to lay a photo out before
// its content is downloaded. Always posting to /upload/file - as the bridge
// used to - stored every desktop picture as a dimensionless generic file.
// "Send as file" deliberately stays generic: it must keep the original bytes.
// kind is what the sender meant, and only it separates a voice message from a
// music file that shares the same MIME. fxl-web puts voice messages in their
// own store through this very type (uploadFoldersByAudioMessage in
// api/files.go), so a FoxMes voice has to land in the same place.
// It also outranks the MIME: an .mkv is video/x-matroska but upstream never
// calls it a video (localimageloader.cpp CheckForVideo), so it is sent as a
// document - and reading the MIME instead put it in the video lane, where the
// chat expects a file it can store as is.
QString UploadTypeFor(const QString &mime, bool forceFile, const QString &kind) {
    if (kind == u"voice"_q) {
        return u"audio-message"_q;
    } else if (kind == u"wallpaper"_q) {
        // Its own store (uploadFoldersByWallpaperChat), and its own lifetime:
        // a wallpaper is never referenced from a message, so the server keeps
        // it active instead of waiting for a send to confirm it.
        return u"wallpaper-chat"_q;
    } else if (forceFile) {
        return u"file"_q;
    } else if (mime == u"image/gif"_q) {
        // The one send the "gif" lane exists for. The chat never stores a gif
        // as a gif: the server renders it to mp4 there and keeps calling the
        // result a gif, which is also what puts it in the store the saved-GIF
        // list reads. An mp4 that is merely gif-like takes the "video" lane
        // below - it is already what the chat stores, and this lane would put
        // it under the duration cap that only a rasterized gif needs.
        return u"gif"_q;
    } else if (kind == u"document"_q) {
        return u"file"_q;
    } else if (kind == u"photo"_q) {
        return u"image"_q;
    } else if (kind == u"video"_q
        || kind == u"video_note"_q
        || kind == u"animation"_q) {
        // A GIF-like send is already an mp4 here, and the "gif" lane would put
        // it under the 60 second cap that only a rasterized .gif needs.
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

// Dev endpoints (FOXMES_URL) serve certificates that do not match the host, so
// peer verification is disabled for every request the bridge makes there:
// REST, upload, fxl-cdn download and the WebSocket handshake. Without
// FOXMES_URL this is a no-op and TLS keeps its default strictness.
void ApplyDevTls(QNetworkRequest &request) {
    if (!DevInsecureTls()) {
        return;
    }
    auto configuration = request.sslConfiguration();
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(configuration);
}

} // namespace

ApiClient::ApiClient(QUrl baseUrl, QObject *parent)
: QObject(parent)
, _baseUrl(normalizeBase(std::move(baseUrl))) {
    if (DevInsecureTls()) {
        // VerifyNone already covers the handshake, this only makes sure a
        // reply is never left hanging on an ssl error we chose to ignore.
        QObject::connect(
            &_network,
            &QNetworkAccessManager::sslErrors,
            this,
            [](QNetworkReply *reply, const QList<QSslError> &) {
                reply->ignoreSslErrors();
            });
    }
}

void ApiClient::setTokens(QString access) {
    _accessToken = std::move(access);
}

void ApiClient::clearTokens() {
    _accessToken.clear();
    _meId = 0;
    if (_tokensChanged) _tokensChanged();
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
	if (authorize && !_accessToken.isEmpty()) {
        result.setRawHeader("Authorization", "Bearer " + _accessToken.toUtf8());
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
	auto request = makeRequest(path, true, authorize, lane, timeoutMs);
    const auto payload = body.isNull() ? QByteArray() : body.toJson(QJsonDocument::Compact);
    QNetworkReply *reply = nullptr;
    if (method == "GET") {
        reply = _network.get(request);
    } else if (method == "POST") {
        reply = _network.post(request, payload);
    } else if (method == "PATCH") {
        reply = _network.sendCustomRequest(request, "PATCH", payload);
    } else if (method == "PUT") {
        reply = _network.sendCustomRequest(request, "PUT", payload);
    } else if (method == "DELETE") {
        reply = _network.sendCustomRequest(request, "DELETE", payload);
    }
    if (!reply) {
        if (done) done({}, u"unsupported HTTP method"_q, 0);
        return;
    }

    finish(reply, std::move(done), lane);
}

void ApiClient::acceptTokens(const QJsonDocument &document) {
    if (!document.isObject()) return;
    const auto object = document.object();
    const auto access = object.value("access_token").toString();
    if (!access.isEmpty()) _accessToken = access;
    const auto user = object.value("user").toObject();
    if (!user.isEmpty()) _meId = user.value("id").toVariant().toLongLong();
    if (_tokensChanged) _tokensChanged();
}

void ApiClient::startDevice(Callback done) {
    // The device name is stored in auth.user_agent so desktop sessions can be
    // distinguished from browser sessions. The server limits its length and
    // strips control characters; use the default if formatting fails.
    auto device = QSysInfo::prettyProductName();
    if (device.isEmpty()) device = u"Unknown OS"_q;
    device += u" FoxMes Desktop"_q;
    jsonRequest("POST", "/auth/device/start", QJsonDocument(QJsonObject{
        {"device_name", device},
    }),
        [done = std::move(done)](QJsonDocument doc, QString err, int status) mutable {
        if (done) done(std::move(doc), std::move(err), status);
    }, false);
}

void ApiClient::exchangeDevice(const QString &request, const QString &code, Callback done) {
    jsonRequest("POST", "/auth/device/exchange", QJsonDocument(QJsonObject{
        {"request", request},
        {"code", code},
    }), [this, done = std::move(done)](QJsonDocument doc, QString err, int status) mutable {
        if (err.isEmpty()) acceptTokens(doc);
        if (done) done(std::move(doc), std::move(err), status);
    }, false);
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

void ApiClient::updateMe(const QString &displayName, Callback done) {
    // PATCH /me was replaced by PUT /me: only display_name is updated.
    jsonRequest("PUT", "/me", QJsonDocument(QJsonObject{
        {"display_name", displayName},
    }), std::move(done));
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

void ApiClient::chats(Callback done) {
    jsonRequest("GET", "/chats", {}, std::move(done));
}

void ApiClient::chatsLight(Callback done) {
    // Startup chat list: no members fan-out, no settings/created_at queries.
    jsonRequest("GET", "/chats?light=1", {}, std::move(done));
}

void ApiClient::chat(qint64 chatId, Callback done) {
    jsonRequest("GET", QString("/chats/%1").arg(chatId), {}, std::move(done));
}

void ApiClient::savedChat(Callback done) {
    jsonRequest("GET", "/chats/saved", {}, std::move(done));
}

void ApiClient::createDirect(qint64 userId, Callback done) {
    jsonRequest("POST", "/chats/direct", QJsonDocument(QJsonObject{
        {"user_id", userId},
    }), std::move(done));
}

void ApiClient::forwardMessages(
        qint64 chatId,
        qint64 sourceChatId,
        const QList<qint64> &messageIds,
        const QString &operationId,
        Callback done) {
    jsonRequest("POST", QString("/chats/%1/messages/forward").arg(chatId), QJsonDocument(QJsonObject{
        {"source_chat_id", sourceChatId},
        {"message_ids", idArray(messageIds)},
        {"operation_id", operationId},
    }), std::move(done));
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

// The schedule is written the way the server reads it: exactly one of the two
// fields, never both. "Without sound" is orthogonal and rides along.
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

} // namespace

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
        // Formatting is stored as marks of the document, so it only travels
        // when there is any: a plain message keeps the exact shape it had.
        body.insert("entities", entities);
    }
    if (clearDraft && clearDraftRevision > 0) {
        // Conditional cleanup: only the draft version observed while typing
        // is removed, so a nonce retry never deletes a newer draft.
        body.insert("clear_draft_revision", clearDraftRevision);
    }
    // A schedule never reaches this endpoint: a scheduled send is a reminder,
    // and the composer routes it to createReminder instead. Only the sound
    // choice belongs on a message that is going out now.
    if (options.silent) {
        body.insert("silent", true);
    }
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
    // The server rebuilds the whole document from text and entities, so an
    // absent array and an empty one mean the same thing - an edit that left
    // no formatting - and the field is simply omitted when there is none.
    if (!entities.isEmpty()) {
        body.insert("entities", entities);
    }
    jsonRequest("PATCH", QString("/messages/%1").arg(messageId), QJsonDocument(body), std::move(done));
}

void ApiClient::deleteMessage(qint64 messageId, Callback done) {
    // Deletion is always global: FoxMes has no per-user hiding, so there is
    // no revoke flag to pass.
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

void ApiClient::react(qint64 messageId, const QString &emoji, Callback done) {
    jsonRequest("POST", QString("/messages/%1/reactions").arg(messageId), QJsonDocument(QJsonObject{
        {"emoji", emoji},
    }), std::move(done));
}

void ApiClient::setReactions(
        qint64 messageId,
        const QStringList &reactions,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done) {
    // Canonical v2 field is "reactions"; the server rejects the legacy
    // "emojis" alias with 400 (DisallowUnknownFields).
    jsonRequest("PUT", QString("/messages/%1/reactions").arg(messageId), QJsonDocument(QJsonObject{
        {"reactions", stringsArray(reactions)},
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

void ApiClient::defaultNotificationSettings(Callback done) {
    jsonRequest("GET", u"/notification-settings/defaults"_q, {}, std::move(done));
}

void ApiClient::setDefaultNotificationSettings(
        qint64 muteUntil,
        bool soundNone,
        const QString &operationId,
        Callback done) {
    jsonRequest("PUT", u"/notification-settings/defaults"_q, QJsonDocument(QJsonObject{
        {"scope", "user"},
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

// The full ordered list of pinned chats. The server assigns dense ranks in a
// transaction and unpins chats that are absent from the list.
void ApiClient::savePinnedOrder(const QList<qint64> &orderedIds, Callback done) {
    auto ids = QJsonArray();
    for (const auto id : orderedIds) {
        ids.append(qint64(id));
    }
    jsonRequest("PUT", QStringLiteral("/chats/list-pin-order"), QJsonDocument(QJsonObject{
        {"ordered_ids", ids},
    }), std::move(done));
}

// Wallpapers. The gallery is the caller's own uploads; the choice lives in the
// per-chat and per-user settings and is answered back authoritatively.
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

// An empty sha256 is the documented reset: the chat follows the user default
// again. There is no "no wallpaper at all in this chat" state.
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

// Saved GIFs, the bridge counterpart of messages.getSavedGifs/saveGif.
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
        Callback done) {
    jsonRequest("POST", QStringLiteral("/gifs"), QJsonDocument(QJsonObject{
        {"chat_id", chatId},
        {"sha256", sha256},
        {"operation_id", operationId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : operationId},
    }), std::move(done));
}

void ApiClient::deleteSavedGif(qint64 gifId, Callback done) {
    jsonRequest("DELETE", QString("/gifs/%1").arg(gifId), {}, std::move(done));
}

ApiClient::CancelHandle ApiClient::uploadDevice(
        const QString &name,
        QIODevice *device,
        const QString &mime,
        qint64 chatId,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    if (!device) {
        if (done) done({}, u"file device is null"_q, 0);
        return {};
    }
    auto multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QString("form-data; name=\"file\"; filename=\"%1\"")
            .arg(name.isEmpty() ? u"upload.bin"_q : name));
    if (!mime.isEmpty()) {
        part.setHeader(QNetworkRequest::ContentTypeHeader, mime);
    }
    part.setBodyDevice(device);
    device->setParent(multipart);
    multipart->append(part);

    QUrlQuery query;
    if (chatId > 0) {
        query.addQueryItem(u"chatId"_q, QString::number(chatId));
    }
    const auto base = QString::fromLatin1(kUploadPath)
        + UploadTypeFor(mime, forceFile, kind);
    const auto path = query.isEmpty()
        ? base
        : base + u"?"_q + query.toString(QUrl::FullyEncoded);
    // One request, so nothing has to be pinned to a pod - the lane is carried
    // only to keep this path identical to the chunk one against the api route
    // table, which pins /upload/ as well.
    const auto lane = std::make_shared<UploadLane>();
    auto request = makeRequest(path, false, true, lane);
    auto reply = _network.post(request, multipart);
    multipart->setParent(reply);
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

// Server bounds for a chunk session (api/files_chunk.go): a part is 64 KB to
// 32 MB and there are at most 1024 of them.
constexpr auto kChunkMin = qint64(64 * 1024);
constexpr auto kChunkMax = qint64(32 * 1024 * 1024);
constexpr auto kChunkCountMax = qint64(1024);
constexpr auto kChunkPreferred = qint64(4 * 1024 * 1024);
// The server abandons a processing job that has not been polled for 10s
// (api/files_hls.go hlsAbandonTimeout), so stay well inside that.
constexpr auto kProcessingPollMs = 2000;
// A part that failed is retried in place, the same way fxl-web does it
// (upload-handler.js). Without this a single blip on part 500 of 640 threw
// away the whole gigabyte behind it.
constexpr auto kChunkRetries = 3;
constexpr auto kChunkRetryBackoffMs = 600;
// When the retries of one part are spent, the session is re-synced against
// the server once or twice instead of failing: action=status says which parts
// actually survived, so a long break costs the missing parts, not all of them.
constexpr auto kChunkResyncs = 2;
// The session lives on one pod's disk and the lane pins the chain to it, so a
// pod that goes away takes the session with it and every further request gets
// "unknown upload session". That is not a verdict on the file - starting over
// works - so the upload re-inits once instead of throwing the user's pick
// away. Once, because a server that keeps losing sessions would otherwise
// loop over the same gigabytes forever.
constexpr auto kUploadRestarts = 1;
// Committing and its background job are the only steps that legitimately go
// quiet for minutes: the server is hashing gigabytes and streaming them into
// object storage.
constexpr auto kCommitTimeoutMs = 5 * 60'000;

[[nodiscard]] qint64 ChunkSizeFor(qint64 size) {
    // Big files raise the part size rather than the part count, because the
    // count is what the server caps.
    auto result = std::max(kChunkPreferred, (size + kChunkCountMax - 1) / kChunkCountMax);
    return std::clamp(result, kChunkMin, kChunkMax);
}

} // namespace

// Sequential chunk pipeline. Everything it needs lives in one shared state, so
// the callbacks can outlive any single request and cancelling is just a flag
// plus an abort of whatever is in flight.
struct ApiClient::ChunkedUpload {
    std::unique_ptr<QIODevice> device;
    QString name;
    QString mime;
    QString type;
    QString uploadId;
    // Set only when the server moved the upload to background processing.
    QString processId;
    qint64 size = 0;
    qint64 chunkSize = 0;
    qint64 sent = 0;
    int totalChunks = 0;
    int index = 0;
    int attempt = 0;
    int resyncs = 0;
    int restarts = 0;
    qint64 chatId = 0;
    // What the server already holds. Seeded by action=status and refreshed by
    // it after a break, so a resumed upload skips what it would re-send.
    std::vector<bool> received;
    bool cancelled = false;
    bool finished = false;
    QPointer<QNetworkReply> reply;
    // Set by the init answer and sent back by every following request, so the
    // whole chain stays on the pod that holds the session.
    LanePtr lane = std::make_shared<UploadLane>();
    Callback done;
    ProgressCallback progress;

    [[nodiscard]] qint64 chunkBytesAt(int chunkIndex) const {
        const auto offset = qint64(chunkIndex) * chunkSize;
        return std::min(chunkSize, size - offset);
    }

    // Progress has to count what the server already had, or a resumed upload
    // would start its bar from zero.
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
        qint64 chatId,
        bool forceFile,
        Callback done,
        ProgressCallback progress,
        const QString &kind) {
    const auto type = UploadTypeFor(mime, forceFile, kind);
    // "image" has no chunk endpoint (api/files_chunk.go:132) and needs the
    // sanitizer that only the direct path runs. A wallpaper is an image by
    // another name and takes the same route, for the same reason: the
    // sanitizer is what writes files_real.width/height, and without those the
    // gallery lays every picture out as a square.
    if (type == u"image"_q || type == u"wallpaper-chat"_q) {
        return uploadDevice(name, device, mime, chatId, forceFile, std::move(done), std::move(progress), kind);
    }
    return uploadDeviceChunked(name, device, mime, chatId, type, std::move(done), std::move(progress));
}

ApiClient::CancelHandle ApiClient::uploadDeviceChunked(
        const QString &name,
        QIODevice *device,
        const QString &mime,
        qint64 chatId,
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
    // Sized up front so a part that succeeds is recorded even when the very
    // first status query fails and leaves nothing to seed it from.
    state->received.assign(state->totalChunks, false);

    state->chatId = chatId;
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
        // The session holds a slot of the user (chunkMaxUserActive) and its
        // parts on the pod's disk until it expires, so a cancelled upload has
        // to hand it back: two cancels in a row otherwise answered the next
        // upload with 429 for the whole session lifetime. Best effort - the
        // upload is already over for the user either way. A session that
        // moved to the background job belongs to it, and its reaper handles
        // the abandoned case.
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
    if (state->chatId > 0) {
        query.addQueryItem(u"chatId"_q, QString::number(state->chatId));
    }
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

// Asks the server which parts it already holds. A failure here is not fatal:
// the worst case is re-sending a part the server could have skipped, which is
// exactly what fxl-web does with the same call.
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
        // A chunk POST cannot report a lost session: the server answers 404
        // before it has read the multi-megabyte body and closes, so the client
        // only ever sees a dropped connection. This query carries no body, so
        // its 404 is the one reliable place to learn the session is gone.
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
    request.setHeader(
        QNetworkRequest::ContentTypeHeader,
        u"application/octet-stream"_q);
    request.setRawHeader("X-Upload-ID", state->uploadId.toUtf8());
    request.setRawHeader("X-Chunk-Index", QByteArray::number(state->index));
    request.setRawHeader("X-Total-Chunks", QByteArray::number(state->totalChunks));
    const auto reply = _network.post(request, body);
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
    // Report the bytes of the chunk in flight on top of the finished ones, so
    // the bar moves inside a part and not only between parts.
    QObject::connect(reply, &QNetworkReply::uploadProgress, this, [state](qint64 sent, qint64) {
        if (state->progress && !state->cancelled) {
            state->progress(std::min(state->sent + sent, state->size), state->size);
        }
    });
}

// The session is gone from the server (a pod that held it went away): the
// bytes on this side are untouched, so the upload starts over instead of
// giving up. The lane goes with it - it points at the pod that is no longer
// there.
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

// A failed part is re-sent in place; the server writes every part at its own
// offset, so repeating one is harmless even when the previous attempt did
// arrive. Only when the retries are spent does the upload ask the server what
// it actually holds and carry on from there.
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
                // A video answers the last step with {processing, processId}
                // and no file at all: the bytes are there, but the HLS
                // rendition that becomes the actual file is still being made.
                // Treating that answer as the upload result gave "attachment
                // upload returned no id" for every video.
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
        // Handing the bytes over is only half the wait: the server still has
        // to stream them into object storage, and for a multi-gigabyte file
        // that is the longer half. Without feeding its percent back the row
        // sat at "1549 / 1549 MB" for minutes and read as a dead transfer.
        if (state->progress && !object.value("done").toBool()) {
            const auto percent = std::clamp(
                object.value("progress").toInt(),
                0,
                100);
            state->progress(state->size * percent / 100, state->size);
        }
        if (object.value("done").toBool()) {
            // The status answer carries the finished file in the same "data"
            // shape the direct upload uses, so it is handed over unchanged.
            state->complete(std::move(doc), QString(), status);
            return;
        }
        if (!weak) {
            state->complete({}, u"upload cancelled"_q, 0);
            return;
        }
        // The server drops a job nobody asks about, so the next poll has to
        // come well inside that window.
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
    const auto effectiveMime = mime.isEmpty()
        ? QMimeDatabase().mimeTypeForFile(filePath).name()
        : mime;
    auto file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        if (done) done({}, file->errorString(), 0);
        delete file;
        return {};
    }
    return uploadPrepared(QFileInfo(filePath).fileName(), file, effectiveMime, chatId, forceFile, std::move(done), std::move(progress), kind);
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
    const auto effectiveMime = mime.isEmpty() ? u"application/octet-stream"_q : mime;
    auto buffer = new QBuffer();
    buffer->setData(data);
    if (!buffer->open(QIODevice::ReadOnly)) {
        if (done) done({}, u"failed to open upload buffer"_q, 0);
        delete buffer;
        return {};
    }
    return uploadPrepared(name, buffer, effectiveMime, chatId, forceFile, std::move(done), std::move(progress), kind);
}

void ApiClient::downloadFile(const QString &path, BytesCallback done) {
    auto reply = _network.get(makeRequest(path, false));
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
    if (!_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + _accessToken.toUtf8());
    }
    ApplyDevTls(request);
    auto reply = _network.get(request);
    QObject::connect(reply, &QNetworkReply::finished, this,
        [reply, done = std::move(done)]() mutable {
            const auto status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto body = reply->readAll();
            QString error;
            if (reply->error() != QNetworkReply::NoError) {
                error = errorFrom(body, reply);
            } else if (status >= 300 && status < 400) {
                // Do not follow redirects because the bearer must not leave
                // the original origin. The CDN serves files directly.
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

QNetworkRequest ApiClient::websocketRequest(qint64 since) const {
	auto request = QNetworkRequest(websocketUrl(since));
	request.setTransferTimeout(kRequestTimeoutMs);
	request.setRawHeader("Accept", "application/json");
	if (!_accessToken.isEmpty()) {
		request.setRawHeader("Authorization", "Bearer " + _accessToken.toUtf8());
	}
	ApplyDevTls(request);
	return request;
}

} // namespace CustomBackend
