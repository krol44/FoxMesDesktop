#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QtCore/QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
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

// Per-attachment media metadata that the bytes cannot carry: the kind that
// separates a voice message from music or a round video note from a video,
// plus playback length, the voice waveform, the track performer and the
// tap-to-reveal spoiler flag.
struct AttachmentMeta {
    QString kind;
    qint64 durationMs = 0;
    QString waveform;
    QString performer;
    // The ID3 track title, which the file name is not. The audio node used to
    // keep the file name in "title", so a player had nothing else to show.
    QString title;
    bool spoiler = false;
};

// SendOptions carries the per-send choices the composer offers alongside the
// message itself. It is a struct and not more positional parameters because
// every send overload would otherwise have to grow again for the next one, and
// it lives at namespace scope so it can be a default argument of those
// overloads.
struct SendOptions {
    // silent is "send without sound": stored on the message, so the recipient
    // still knows about it after a reconnect.
    bool silent = false;
    // deliverAt is a unix second; non-zero turns the send into a reminder that
    // fires then. deliverWhenOnline waits for the recipient instead. Exactly
    // one of the two is ever set, which is what the server accepts.
    qint64 deliverAt = 0;
    bool deliverWhenOnline = false;

    [[nodiscard]] bool scheduled() const {
        return (deliverAt != 0) || deliverWhenOnline;
    }
};

// Envoy pins a whole upload chain to one fxl-api pod: the chunk session lives
// in that pod's temp dir and process memory, so an init on pod A followed by a
// chunk on pod B answers "unknown upload session". The pin travels as an opaque
// pointer that Envoy puts in the response header and expects back in the next
// request (fxl-infra/k8s/21-envoy-routes.yaml, fxl-api/k8s/README.md). It is
// checked against the cluster's endpoints, so an invented value is ignored
// rather than trusted; the client only carries it. One instance lives for one
// upload chain and is shared by every request of it.
struct UploadLane {
    QByteArray value;
};

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

    // Device pairing replaces password-based login. startDevice returns a URL
    // for fxl.ru/settings/foxMes; exchangeDevice consumes its one-time code.
    // The session is indefinite, like the web session. There is no refresh
    // token or /auth/refresh endpoint.
    void startDevice(Callback done);
    void exchangeDevice(const QString &request, const QString &code, Callback done);
    void logout(Callback done);
    void me(Callback done);
    void updateMe(const QString &displayName, Callback done);

    void users(const QString &query, Callback done);
    void user(qint64 userId, Callback done);
    void reactionsCatalog(Callback done);

    void chats(Callback done);
    void chatsLight(Callback done);
    void chat(qint64 chatId, Callback done);
    void savedChat(Callback done);
    void createDirect(qint64 userId, Callback done);
    void forwardMessages(qint64 chatId, qint64 sourceChatId, const QList<qint64> &messageIds, const QString &operationId, Callback done);
    // Pinned messages are a shared property of the chat. Every response and
    // every chat.pinned event carries the whole authoritative id set plus a
    // monotonic pin_revision, so the client replaces its list instead of
    // patching it. limit only bounds how many hydrated bodies come along.
    void pinnedMessages(qint64 chatId, int limit, Callback done);
    // forEveryone mirrors the "Also pin for {user}" checkbox: false pins only
    // for the caller, true pins for the whole chat. Upstream expresses the
    // same choice inverted, as f_pm_oneside.
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
    // forceFile marks the whole send as "send as file" so the server stores
    // the attachments as file nodes instead of picking by MIME.
    // posters maps an attachment id to the poster url its upload produced;
    // only a video has one, and the server cannot look it up itself.
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
    // One item per attachment: a native message holds exactly one media, so
    // an album is a group of messages sharing a grouped_id, not one message
    // with several attachments. Text and entities ride on the first item.
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
    // Same endpoint with the conditional draft cleanup: only the observed
    // draft version is cleared after a successful send.
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

    // Reminders are scheduled messages: they live in their own queue until
    // they fire, and are then delivered by the server through the same send
    // path a live message takes.
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
    // Rescheduling and editing the text are the same mutation: both leave the
    // reminder in the queue, and both bump its revision.
    void updateReminder(
        qint64 reminderId,
        const QString &text,
        const QJsonArray &entities,
        const SendOptions &options,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done);
    // all cancels the whole album the reminder belongs to: a scheduled album
    // is one item to the user, so half of it is not a state they can reach.
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
    // Deletion is always global; there is no per-user hiding in FoxMes.
    void deleteMessage(qint64 messageId, Callback done);
    // Global-only batch delete (the desktop client always deletes for
    // everyone): one transactional SQL pass; operationId keeps retries
    // idempotent via the server-side operation journal.
    void deleteMessages(
        qint64 chatId,
        const QList<qint64> &messageIds,
        const QString &operationId,
        Callback done);
    void deleteHistory(qint64 chatId, Callback done);
    void deleteMessagesByDate(qint64 chatId, qint64 minDate, qint64 maxDate, Callback done);
    void deleteChat(qint64 chatId, Callback done);
    void react(qint64 messageId, const QString &emoji, Callback done);
    // Guarded full-set replace (v2): the whole set is sent with the
    // reaction_revision observed by the caller; a mismatch answers 409 with
    // the authoritative message.
    void setReactions(
        qint64 messageId,
        const QStringList &reactions,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done);
    // Ambiguous-timeout recovery: stored result of a journaled mutation.
    void operationResult(const QString &operationId, Callback done);
    void markRead(qint64 chatId, qint64 messageId, Callback done = {});
    // First-touch delivery acknowledgement for messages that actually reached
    // this device, batched by the bridge.
    void markDelivered(
        qint64 chatId,
        const QList<qint64> &messageIds,
        Callback done = {});
    void typing(qint64 chatId, Callback done = {});
    // Link preview for the composer field: the server reads the page, so the
    // client never touches a third-party host itself. Mirrors
    // messages.getWebPagePreview.
    void linkPreview(const QString &url, Callback done);
    void searchMessages(
        const QString &query,
        qint64 chatId,
        qint64 before,
        int limit,
        Callback done);
    // One shared media list of a chat (Photos, Videos, Links, Files, Music,
    // Voice). Anchors follow the message page contract: they are positions,
    // and each edge reports its own exhaustion.
    // The same media lists across every chat of the account: what the media
    // tabs of the chat-list search show. Ordering is by message id, which is
    // global, so one before cursor is enough.
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
    // v2 drafts: optimistic concurrency. base_revision guards against a
    // concurrent device win; operation_id makes retries idempotent.
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
    // Per-user notification defaults: what upstream saves through
    // account.updateNotifySettings with inputNotifyUsers. Only the private
    // chats scope is served, so no scope argument is exposed here.
    void defaultNotificationSettings(Callback done);
    void setDefaultNotificationSettings(
        qint64 muteUntil,
        bool soundNone,
        const QString &operationId,
        Callback done);
    void setChatPinned(qint64 chatId, bool pinned, Callback done);
    void setChatUnreadMark(qint64 chatId, bool markedUnread, Callback done);
    void setChatArchived(qint64 chatId, bool archived, Callback done);
    void savePinnedOrder(const QList<qint64> &orderedIds, Callback done);

    // Wallpapers. Upstream reads these from account.getWallPapers and writes
    // the per-chat choice with messages.setChatWallPaper; neither exists under
    // the bridge, so the gallery is the caller's own uploads and the choice
    // rides with the rest of the per-chat settings.
    void wallpapers(Callback done);
    void deleteWallpaper(const QString &sha256, Callback done);
    void defaultWallpaper(Callback done);
    // blurred and intensity belong to the choice, not to the file: the same
    // picture is blurred in one chat and sharp in another, and the dimming
    // slider is per chat too. Upstream keeps both on the WallPaper for the
    // same reason.
    void setDefaultWallpaper(
        const QString &sha256,
        bool blurred,
        int intensity,
        const QString &operationId,
        Callback done);
    // An empty sha256 resets the chat to the user default.
    // forBoth applies the picture to the other side of the conversation too.
    void setChatWallpaper(
        qint64 chatId,
        const QString &sha256,
        bool blurred,
        int intensity,
        bool forBoth,
        const QString &operationId,
        Callback done);

    // "No background in this chat" - a state of its own, not the same as
    // clearing the chat's picture, which only brings back the account default.
    void setChatNoWallpaper(
        qint64 chatId,
        const QString &operationId,
        Callback done);

    // The chat theme, named by its emoji. An empty value is "no theme". The
    // palette behind the name is compiled into the client, so only the name
    // travels.
    void setChatTheme(
        qint64 chatId,
        const QString &emoticon,
        const QString &operationId,
        Callback done);

    // Saved GIFs: messages.getSavedGifs / messages.saveGif under the bridge.
    // chatId on saveGif is where the picture was seen - it is what lets the
    // server copy somebody else's file to the caller.
    void savedGifs(int limit, qint64 beforeId, Callback done);
    void saveGif(
        qint64 chatId,
        const QString &sha256,
        const QString &operationId,
        Callback done);
    void deleteSavedGif(qint64 gifId, Callback done);

    // Upload through the canonical fxl-api /upload/{type} flow, scoped to a
    // chat by chatId. The response is {ok, data:{id, url, sha256, ...}}.
    // The {type} segment is picked from the MIME: only the typed image
    // endpoint runs the sanitizer that stores width/height, and forceFile
    // keeps a "send as file" upload byte-identical by staying on /upload/file.
    using ProgressCallback = std::function<void(qint64 sent, qint64 total)>;
    // Stops the transfer. Safe to call after it already finished.
    using CancelHandle = std::function<void()>;
    CancelHandle uploadFile(const QString &filePath, const QString &mime, qint64 chatId, bool forceFile, Callback done, ProgressCallback progress = {}, const QString &kind = QString());
    CancelHandle uploadData(const QString &name, const QByteArray &data, const QString &mime, qint64 chatId, bool forceFile, Callback done, ProgressCallback progress = {}, const QString &kind = QString());
    void downloadFile(const QString &path, BytesCallback done);
    // GET an absolute fxl-cdn URL with bearer authorization. Redirects are not
    // followed so Authorization cannot be sent to another origin.
    void downloadUrl(const QUrl &url, BytesCallback done);
    [[nodiscard]] QUrl websocketUrl(qint64 since) const;
    [[nodiscard]] QNetworkRequest websocketRequest(qint64 since) const;

private:
    // timeoutMs is the inactivity budget of a single request. It is raised
    // only for the two steps that legitimately go quiet for minutes: the
    // final commit of a chunked upload and the poll of its background job.
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
    struct ChunkedUpload;
    void sendNextChunk(std::shared_ptr<ChunkedUpload> state);
    void finishChunkedUpload(std::shared_ptr<ChunkedUpload> state);
    // A video upload does not end with the last chunk: the server answers the
    // "complete" action with {processing, processId} and transcodes to HLS in
    // the background, publishing the file only when that is done. The upload
    // is only finished once this poll reports done, and the server abandons a
    // job that is not polled, so the wait is an active one.
    void pollChunkedProcessing(std::shared_ptr<ChunkedUpload> state);
    // Opens a chunk session (action=init) and starts sending parts.
    void startChunkSession(std::shared_ptr<ChunkedUpload> state);
    // Starts the whole upload over when the server lost the session.
    bool restartChunkedUpload(
        std::shared_ptr<ChunkedUpload> state,
        int status);
    // Reads back which parts the server holds (action=status) and continues.
    void syncChunkedUpload(
        std::shared_ptr<ChunkedUpload> state,
        Fn<void()> then);
    // Re-sends the failed part, then falls back to a full re-sync.
    void retryChunk(
        std::shared_ptr<ChunkedUpload> state,
        QString error,
        int status);
    void finish(
        QNetworkReply *reply,
        Callback done,
        const LanePtr &lane = nullptr);
    void acceptTokens(const QJsonDocument &document);
    // Direct single-request upload. Only "image" goes this way: the chunk
    // endpoint rejects that type, and only this path runs the sanitizer that
    // fills files_real.width/height.
    CancelHandle uploadDevice(const QString &name, QIODevice *device, const QString &mime, qint64 chatId, bool forceFile, Callback done, ProgressCallback progress, const QString &kind);
    // Canonical streaming upload: /uploadChunk/{type}?action=init|chunk|
    // complete. Takes ownership of the device and never holds more than one
    // chunk in memory, so a multi-gigabyte file costs a buffer, not its size.
    // Routes to the chunk flow, or to uploadDevice() for "image".
    CancelHandle uploadPrepared(const QString &name, QIODevice *device, const QString &mime, qint64 chatId, bool forceFile, Callback done, ProgressCallback progress, const QString &kind);
    CancelHandle uploadDeviceChunked(const QString &name, QIODevice *device, const QString &mime, qint64 chatId, const QString &type, Callback done, ProgressCallback progress);

    QUrl _baseUrl;
    QString _accessToken;
    qint64 _meId = 0;
    qint64 _eventSequence = 0;
    TokensChanged _tokensChanged;
    QNetworkAccessManager _network;
};

} // namespace CustomBackend
