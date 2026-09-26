#pragma once

#include <functional>
#include <optional>
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

struct SendOptions {
    bool silent = false;
    // deliverAt is a unix second; non-zero turns the send into a reminder that
    // fires then. deliverWhenOnline waits for the recipient instead. Exactly
    // one of the two is ever set, which is what the server accepts.
    qint64 deliverAt = 0;
    bool deliverWhenOnline = false;

    // mediaTtlSeconds arms disappearing media. 1..60 is a timer;
    // kMediaTtlOnce is "view once". Zero is an ordinary message. The value
    // comes from Ui::PreparedFile::ttlSeconds and from the voice/round record
    // bar, which both already speak the upstream sentinel.
    int mediaTtlSeconds = 0;

    [[nodiscard]] bool scheduled() const {
        return (deliverAt != 0) || deliverWhenOnline;
    }
    [[nodiscard]] bool ephemeral() const {
        return mediaTtlSeconds != 0;
    }
};

// kMediaTtlOnce mirrors the upstream sentinel both pickers already emit for
// "view once" (Data::kMaxTtlSeconds). The bridge keeps it as-is on the wire:
// the server accepts it as an inbound alias of media_ttl_mode = "once".
inline constexpr auto kMediaTtlOnce = 2147483647;

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
    // for foxtail.ing/settings/foxMes; exchangeDevice consumes its one-time
    // code.
    // The session is indefinite, like the web session. There is no refresh
    // token or /auth/refresh endpoint.
    void startDevice(Callback done);
    void exchangeDevice(const QString &request, const QString &code, Callback done);
    void logout(Callback done);
    void me(Callback done);
    void updateMe(const QString &displayName, Callback done);

    // The desktop version an administrator allowed to roll out. Sent without a
    // bearer on purpose: the update button exists on the login screen too, and
    // a client that is not paired yet is exactly the one running an old build.
    // Nothing is fetched from GitHub until this says a newer build exists.
    void desktopVersion(Callback done);

    void users(const QString &query, Callback done);
    void user(qint64 userId, Callback done);
    void reactionsCatalog(Callback done);
    void reactionUsage(Callback done);

    void chats(Callback done);
    void chatsLight(Callback done);
    void chat(qint64 chatId, Callback done);
    void savedChat(Callback done);
    void createDirect(qint64 userId, Callback done);
    void forwardMessages(qint64 chatId, qint64 sourceChatId, const QList<qint64> &messageIds, bool dropAuthor, std::optional<int> videoTimestamp, const QString &operationId, Callback done);
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

    // Disappearing media: the equivalent of messages.readMessageContents.
    // One call, sent when the content is actually shown - it both spends a
    // "view once" and starts the countdown. The attachment itself rides in the
    // message like any other, so there is nothing else to ask for.
    void markEphemeralViewed(qint64 messageId, Callback done);

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
    // Guarded full-set replace (v2): the whole set is sent with the
    // reaction_revision observed by the caller; a mismatch answers 409 with
    // the authoritative message. The set is catalog ids - the emoji itself
    // repeats between catalog groups and names no row.
    void setReactions(
        qint64 messageId,
        const std::vector<DocumentId> &reactions,
        qint64 expectedRevision,
        const QString &operationId,
        Callback done);
    // Ambiguous-timeout recovery: stored result of a journaled mutation.
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
    // Group calls. The conference adapter owns the paths and bodies: it
    // answers upstream's phone.* requests one to one, and a method per route
    // here would only repeat them.
    void conferenceRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonObject &body,
        Callback done);
    // Groups and channels: the channels adapter answers upstream's channels.*
    // one to one and owns the paths and bodies, like the conference adapter.
    void communityRequest(
        const QByteArray &method,
        const QString &path,
        const QJsonObject &body,
        Callback done);
    // PUT /chats/{id}/photo - an image as multipart "file".
    void communityPhoto(qint64 chatId, const QByteArray &jpeg, Callback done);
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
