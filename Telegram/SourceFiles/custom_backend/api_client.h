#pragma once

#include <functional>
#include <optional>
#include <memory>

#include <QByteArray>
#include <QtCore/QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>

#include "data/data_messages.h"

class QNetworkReply;

namespace CustomBackend {

struct AttachmentMeta {
    QString kind;
    qint64 durationMs = 0;
    QString waveform;
    QString performer;
    QString title;
    bool spoiler = false;
};

struct UploadTarget {
    qint64 chatId = 0;
    qint64 topicId = 0;
};

[[nodiscard]] QJsonObject AttachmentMetaJson(
    const QMap<qint64, AttachmentMeta> &meta);
[[nodiscard]] QJsonObject AttachmentPostersJson(
    const QMap<qint64, QString> &posters);

struct SendOptions {
    bool silent = false;
    qint64 deliverAt = 0;
    bool deliverWhenOnline = false;

    int mediaTtlSeconds = 0;

    [[nodiscard]] bool scheduled() const {
        return (deliverAt != 0) || deliverWhenOnline;
    }
    [[nodiscard]] bool ephemeral() const {
        return mediaTtlSeconds != 0;
    }
};

inline constexpr auto kMediaTtlOnce = 2147483647;

struct UploadLane {
    QByteArray value;
};

[[nodiscard]] bool IsTransportFailure(int status);

class ApiClient final : public QObject {
public:
    using Callback = std::function<void(QJsonDocument, QString, int)>;
    using LanePtr = std::shared_ptr<UploadLane>;
    using BytesCallback = std::function<void(QByteArray, QString, int)>;
    using TokensChanged = std::function<void()>;

    explicit ApiClient(QUrl baseUrl, QObject *parent = nullptr);

    [[nodiscard]] QUrl baseUrl() const { return _baseUrl; }
    [[nodiscard]] QString accessToken() const { return _accessToken; }
    [[nodiscard]] qint64 meId() const { return _meId; }
    [[nodiscard]] qint64 eventSequence() const { return _eventSequence; }
    void setMeId(qint64 id) { _meId = id; }
    void setEventSequence(qint64 value) { _eventSequence = value; }
    void setTokensChangedCallback(TokensChanged callback) { _tokensChanged = std::move(callback); }

    void setTokens(QString access);
    void clearTokens();

    void startDevice(Callback done);
    void exchangeDevice(const QString &request, const QString &code, Callback done);
    void logout(Callback done);
    void me(Callback done);
    void updateMe(const QString &displayName, Callback done);

    void desktopVersion(Callback done);

    void users(const QString &query, Callback done);
    void user(qint64 userId, Callback done);
    void setUserBlocked(qint64 userId, bool blocked, Callback done);
    void setUserContact(qint64 userId, QJsonObject fields, Callback done);
    void reactionsCatalog(Callback done);
    void reactionUsage(Callback done);

    void chats(Callback done);
    void chatsLight(Callback done);
    void chat(qint64 chatId, Callback done);
    void savedChat(Callback done);
    void createDirect(qint64 userId, Callback done);
    void forwardMessages(qint64 chatId, qint64 sourceChatId, qint64 threadRootId, const QList<qint64> &messageIds, bool dropAuthor, std::optional<int> videoTimestamp, const QString &operationId, Callback done);
    void pinnedMessages(qint64 chatId, int limit, Callback done);
    void pinMessage(
        qint64 chatId,
        qint64 messageId,
        bool forEveryone,
        const QString &operationId,
        Callback done);
    void unpinMessage(
        qint64 chatId,
        qint64 messageId,
        const QString &operationId,
        Callback done);
    void unpinAllMessages(
        qint64 chatId,
        const QString &operationId,
        Callback done);

    void messages(
        qint64 chatId,
        qint64 aroundId,
        int limit,
        Data::LoadDirection direction,
        Callback done);
    void message(qint64 chatId, qint64 messageId, Callback done);
	void messageById(qint64 messageId, Callback done);
    void sendMessage(
        qint64 chatId,
        const QString &text,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool clearDraft,
        Callback done,
        bool forceFile = false,
        const QMap<qint64, QString> &posters = {},
        const QMap<qint64, AttachmentMeta> &meta = {});
    struct AlbumItem {
        QString clientNonce;
        qint64 attachmentId = 0;
        AttachmentMeta meta;
    };
    void sendAlbum(
        qint64 chatId,
        const QString &caption,
        const QJsonArray &entities,
        qint64 replyToId,
        const QList<AlbumItem> &items,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        Callback done,
        const SendOptions &options = {});
    void sendMessageAlbum(
        qint64 chatId,
        const QString &caption,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool clearDraft,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const QMap<qint64, AttachmentMeta> &meta,
        Callback done);
    void sendMessageWithDraftRevision(
        qint64 chatId,
        const QString &text,
        qint64 replyToId,
        const QList<qint64> &attachmentIds,
        const QString &clientNonce,
        bool clearDraft,
        qint64 clearDraftRevision,
        Callback done,
        bool forceFile = false,
        const QMap<qint64, QString> &posters = {},
        const QMap<qint64, AttachmentMeta> &meta = {},
        const QJsonArray &entities = {},
        const SendOptions &options = {});

    void markEphemeralViewed(qint64 messageId, Callback done);

    void reminders(qint64 chatId, Callback done);
    void createReminder(
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
        Callback done);
    void createReminderAlbum(
        qint64 chatId,
        const QString &caption,
        const QJsonArray &entities,
        qint64 replyToId,
        const QList<AlbumItem> &items,
        bool forceFile,
        const QMap<qint64, QString> &posters,
        const SendOptions &options,
        const QString &operationId,
        Callback done);
    void updateReminder(
        qint64 reminderId,
        const QString &text,
        const QJsonArray &entities,
        const SendOptions &options,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done);
    void deleteReminder(
        qint64 reminderId,
        bool all,
        const QString &operationId,
        Callback done);
    void sendReminderNow(
        qint64 reminderId,
        const QString &operationId,
        Callback done);

    void editMessage(
        qint64 messageId,
        const QString &text,
        const QJsonArray &entities,
        qint64 expectedRevision,
        Callback done);
    void deleteMessage(qint64 messageId, Callback done);
    void deleteMessages(
        qint64 chatId,
        const QList<qint64> &messageIds,
        const QString &operationId,
        Callback done);
    void deleteHistory(qint64 chatId, Callback done);
    void deleteMessagesByDate(qint64 chatId, qint64 minDate, qint64 maxDate, Callback done);
    void deleteChat(qint64 chatId, Callback done);
    void setReactions(
        qint64 messageId,
        const std::vector<DocumentId> &reactions,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done);
    void operationResult(const QString &operationId, Callback done);
    void markRead(qint64 chatId, qint64 messageId, Callback done = {});
    void createMeet(qint64 chatId, const QString &operationId, Callback done);

    void callConfig(Callback done);
    void callDhConfig(Callback done);
    void requestCall(
        qint64 chatId,
        const QByteArray &gaHash,
        bool video,
        const QJsonObject &protocol,
        const QString &operationId,
        Callback done);
    void callReceived(qint64 callId, Callback done);
    void callAccept(
        qint64 callId,
        const QByteArray &gb,
        const QJsonObject &protocol,
        Callback done);
    void callConfirm(
        qint64 callId,
        const QByteArray &ga,
        qint64 keyFingerprint,
        Callback done);
    void callHeartbeat(qint64 callId, Callback done);
    void callDiscard(
        qint64 callId,
        const QString &reason,
        int duration,
        const QString &slug,
        Callback done);
    void callSignaling(qint64 callId, const QByteArray &data, Callback done);
    void callSettings(Callback done);
    void callSettingsUpdate(const QJsonObject &settings, Callback done);
    void callHistory(qint64 offsetId, int limit, Callback done);
    void callHistoryClear(Callback done);
    void conferenceRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonObject &body,
        Callback done);
    void communityRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonObject &body,
        Callback done);
    void hlsSegments(const QString &playlistSha256, Callback done);
    void communityPhoto(
        qint64 chatId,
        const QByteArray &jpeg,
        const QByteArray &video,
        double videoStartTs,
        Callback done);
    void markDelivered(
        qint64 chatId,
        const QList<qint64> &messageIds,
        Callback done = {});
    void typing(qint64 chatId, Callback done = {});
    void linkPreview(const QString &url, Callback done);
    void searchMessages(
        const QString &query,
        qint64 chatId,
        qint64 before,
        int limit,
        Callback done);
    void globalMedia(
        const QString &kind,
        const QString &query,
        qint64 before,
        int limit,
        Callback done);
    void chatMedia(
        qint64 chatId,
        const QString &kind,
        const QString &query,
        qint64 before,
        qint64 after,
        qint64 around,
        int limit,
        Callback done);
    void draft(qint64 chatId, Callback done);
    void setDraft(
        qint64 chatId,
        const QString &text,
        qint64 replyToId,
        qint64 baseRevision,
        const QString &operationId,
        Callback done);
    void notificationSettings(qint64 chatId, Callback done);
    void setNotificationSettings(
        qint64 chatId,
        qint64 muteUntil,
        bool showPreviews,
        bool soundNone,
        Callback done);
    void defaultNotificationSettings(const QString &scope, Callback done);
    void setDefaultNotificationSettings(
        const QString &scope,
        qint64 muteUntil,
        bool soundNone,
        const QString &operationId,
        Callback done);
    void setChatPinned(qint64 chatId, bool pinned, Callback done);
    void setChatUnreadMark(qint64 chatId, bool markedUnread, Callback done);
    void setChatArchived(qint64 chatId, bool archived, Callback done);
    void savePinnedOrder(const QList<qint64> &orderedIds, Callback done);

    void wallpapers(Callback done);
    void deleteWallpaper(const QString &sha256, Callback done);
    void defaultWallpaper(Callback done);
    void setDefaultWallpaper(
        const QString &sha256,
        bool blurred,
        int intensity,
        const QString &operationId,
        Callback done);
    void setChatWallpaper(
        qint64 chatId,
        const QString &sha256,
        bool blurred,
        int intensity,
        bool forBoth,
        const QString &operationId,
        Callback done);

    void setChatNoWallpaper(
        qint64 chatId,
        const QString &operationId,
        Callback done);

    void setChatTheme(
        qint64 chatId,
        const QString &emoticon,
        const QString &operationId,
        Callback done);

    void savedGifs(int limit, qint64 beforeId, Callback done);
    void saveGif(
        qint64 chatId,
        const QString &sha256,
        const QString &operationId,
        Callback done,
        qint64 messageId = 0,
        qint64 threadRootId = 0);
    void deleteSavedGif(qint64 gifId, Callback done);

    using ProgressCallback = std::function<void(qint64 sent, qint64 total)>;
    using CancelHandle = std::function<void()>;
    CancelHandle uploadFile(const QString &filePath, const QString &mime, qint64 chatId, bool forceFile, Callback done, ProgressCallback progress = {}, const QString &kind = QString());
    CancelHandle uploadData(const QString &name, const QByteArray &data, const QString &mime, qint64 chatId, bool forceFile, Callback done, ProgressCallback progress = {}, const QString &kind = QString());
    CancelHandle uploadFile(const QString &filePath, const QString &mime, const UploadTarget &target, bool forceFile, Callback done, ProgressCallback progress = {}, const QString &kind = QString(), const QString &displayName = QString());
    CancelHandle uploadData(const QString &name, const QByteArray &data, const QString &mime, const UploadTarget &target, bool forceFile, Callback done, ProgressCallback progress = {}, const QString &kind = QString());
    void downloadFile(const QString &path, BytesCallback done);
    void downloadUrl(const QUrl &url, BytesCallback done);
    [[nodiscard]] QUrl websocketUrl(qint64 since) const;
    [[nodiscard]] QNetworkRequest websocketRequest(qint64 since) const;

private:
    QNetworkRequest makeRequest(
        const QString &path,
        bool json = true,
        bool authorize = true,
        const LanePtr &lane = nullptr,
        int timeoutMs = 0) const;
    void jsonRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonDocument &body,
        Callback done,
        bool authorize = true,
        const LanePtr &lane = nullptr,
        int timeoutMs = 0);
    void jsonRequestImpl(
        const QByteArray &method,
        const QString &path,
        const QJsonDocument &body,
        Callback done,
        bool authorize);
    struct ResendingRequest;
    void resendingJsonRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonDocument &body,
        Callback done);
    void sendResending(std::shared_ptr<ResendingRequest> state);
    struct ChunkedUpload;
    void sendNextChunk(std::shared_ptr<ChunkedUpload> state);
    void finishChunkedUpload(std::shared_ptr<ChunkedUpload> state);
    void pollChunkedProcessing(std::shared_ptr<ChunkedUpload> state);
    void startChunkSession(std::shared_ptr<ChunkedUpload> state);
    bool restartChunkedUpload(
        std::shared_ptr<ChunkedUpload> state,
        int status);
    void syncChunkedUpload(
        std::shared_ptr<ChunkedUpload> state,
        Fn<void()> then);
    void retryChunk(
        std::shared_ptr<ChunkedUpload> state,
        QString error,
        int status);
    void finish(
        QNetworkReply *reply,
        Callback done,
        const LanePtr &lane = nullptr);
    void acceptTokens(const QJsonDocument &document);
    CancelHandle uploadDevice(const QString &name, QIODevice *device, const QString &mime, const UploadTarget &target, bool forceFile, Callback done, ProgressCallback progress, const QString &kind);
    CancelHandle uploadPrepared(const QString &name, QIODevice *device, const QString &mime, const UploadTarget &target, bool forceFile, Callback done, ProgressCallback progress, const QString &kind);
    CancelHandle uploadDeviceChunked(const QString &name, QIODevice *device, const QString &mime, const UploadTarget &target, const QString &type, Callback done, ProgressCallback progress);

    QUrl _baseUrl;
    QString _accessToken;
    qint64 _meId = 0;
    qint64 _eventSequence = 0;
    TokensChanged _tokensChanged;
    QNetworkAccessManager _network;
};

}
