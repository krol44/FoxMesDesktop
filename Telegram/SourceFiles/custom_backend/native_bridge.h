#pragma once

#include "api/api_common.h"
#include "custom_backend/api_client.h"
#include "custom_backend/live_updates_connection.h"
#include "data/data_drafts.h"
#include "data/data_messages.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class History;
class HistoryItem;
class PeerData;
enum class NewMessageType;

namespace Main {
class Session;
}

namespace Window {
class SessionController;
}

namespace Api {
struct MessageToSend;
}

// Identity of an in-flight history page request. The full 64-bit chat and
// anchor ids are kept: truncating them to 32 bits made different requests
// collide on large legacy ids.
struct HistoryLoadKey {
	qint64 chatId = 0;
	qint64 aroundId = 0;
	Data::LoadDirection direction = Data::LoadDirection::Before;
	// A fresh-tail request looks identical to an anchorless "load older" one
	// but is applied differently, so the two must not share a slot.
	bool tail = false;

	bool operator==(const HistoryLoadKey &other) const {
		return chatId == other.chatId
			&& aroundId == other.aroundId
			&& direction == other.direction
			&& tail == other.tail;
	}
};

struct HistoryLoadKeyHash {
	std::size_t operator()(const HistoryLoadKey &key) const {
		constexpr auto kMixConstant = std::size_t(0x9e3779b97f4a7c15ULL);
		auto mix = [](std::size_t seed, qint64 value) {
			auto part = static_cast<std::size_t>(value);
			return seed ^ (part + kMixConstant + (seed << 6) + (seed >> 2));
		};
		auto result = std::hash<qint64>()(key.chatId);
		result = mix(result, key.aroundId);
		result = mix(result, static_cast<qint64>(key.direction));
		result = mix(result, key.tail ? 1 : 0);
		return result;
	}
};

// Identity of one message inside one chat. Both ids stay 64-bit: packing them
// into a single integer truncated chats.fox_mes_id (a bigint) to 32 bits, so
// two chats could share seen-state, revisions and read watermarks.
struct MessageKey {
	qint64 chatId = 0;
	qint64 messageId = 0;

	bool operator==(const MessageKey &other) const {
		return chatId == other.chatId && messageId == other.messageId;
	}
};

struct MessageKeyHash {
	std::size_t operator()(const MessageKey &key) const {
		constexpr auto kMixConstant = std::size_t(0x9e3779b97f4a7c15ULL);
		auto result = std::hash<qint64>()(key.chatId);
		const auto part = static_cast<std::size_t>(key.messageId);
		return result
			^ (part + kMixConstant + (result << 6) + (result >> 2));
	}
};

namespace Data {
class Folder;
class Thread;
class DocumentMedia;
enum class DefaultNotify : uint8_t;
}

namespace CustomBackend {

// A reply names its parent by (chat, message). "Reply in Another Chat" puts a
// parent from a different conversation into the composer, and the peer is what
// the optimistic bubble needs to draw the same external reply header the
// server will send back for it. On the wire the id alone is enough - message
// ids are global - so only the local side carries the peer.
struct ReplyTarget {
	qint64 messageId = 0;
	PeerId peer;

	[[nodiscard]] explicit operator bool() const {
		return messageId != 0;
	}
};

class ApiClient;

struct UploadSpec {
    QString path;
    QString displayName;
    QString mime;
    QByteArray content;
    // The user picked "send as file" for this group, so an image must be
    // rendered as a document instead of a photo.
    bool forceFile = false;
    // Media metadata the receiving client cannot derive from the bytes: the
    // same MIME is a voice message or a music file depending on intent, and a
    // video may be an animation or a round video note. Nothing on the server
    // stores it either, so it travels with the send and lands on the document
    // node.
    QString kind;
    qint64 durationMs = 0;
    QString waveform;
    QString performer;
    // The ID3 track title. It is not the file name: a player shows the title
    // and the file name is what a download is called, so both travel.
    QString title;
    // Cover art embedded in the tag, already encoded as an image. It is
    // uploaded as a file of its own and ends up as the document thumbnail,
    // exactly like the preview of a video.
    QByteArray cover;
    bool spoiler = false;
};

// Rendering input that only the sending client has: the bytes already read
// from disk for the optimistic local item, and the "send as file" choice.
// The server message DTO carries neither, so when the sent message comes back
// and replaces the local item both have to be carried over from the pending
// send - otherwise a just-sent photo loses its preview.
struct LocalAttachment {
    QByteArray bytes;
    // Where the file being uploaded already lies on disk, so the optimistic
    // item can be pointed at real content instead of waiting for the upload.
    QString path;
    bool forceFile = false;
};

// Translates the composer's per-send choices into the wire options. Both
// halves of "scheduled" collapse here: upstream marks "send when online" with
// a sentinel timestamp, while the server takes it as a flag of its own.
[[nodiscard]] SendOptions SendOptionsFrom(
    const Api::SendOptions &options);

// The composer's reply, reduced to what the bridge sends and echoes locally.
[[nodiscard]] ReplyTarget ReplyTargetFrom(
    History *history,
    const FullReplyTo &replyTo);

class NativeBridge final : public QObject {
public:
    explicit NativeBridge(Main::Session *session);
    ~NativeBridge() override;

    void reloadChats();
    void loadContacts();
    using HistoryLoaded = std::function<void()>;
    void loadHistory(
        History *history,
        qint64 aroundId = 0,
        Data::LoadDirection direction = Data::LoadDirection::Before,
        HistoryLoaded done = {});
    void sendMessage(
        Api::MessageToSend &&message,
        std::optional<MsgId> localMessageId);
    void saveDraftToCloudDelayed(Data::Thread *thread);
    void sendText(
        History *history,
        const QString &text,
        const EntitiesInText &entities = {},
        Data::WebPageDraft webPage = {},
        ReplyTarget replyTo = {},
        std::optional<MsgId> localMessageId = std::nullopt,
        bool clearDraft = false,
        MsgId draftTopicRootId = MsgId(),
        PeerId draftMonoforumPeerId = PeerId(),
        // Reuses the nonce of an earlier attempt. The server is idempotent per
        // (user, chat, nonce), so a resend of a request whose reply was lost
        // returns the message that already exists instead of duplicating it.
        const QString &reuseClientNonce = QString(),
        const SendOptions &options = {});
    // A scheduled text send. It never creates an optimistic bubble in the
    // chat: the message is not going there yet, and upstream puts a scheduled
    // send into the scheduled list instead - which is also where the composer
    // navigates the moment the send starts.
    void scheduleText(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        ReplyTarget replyTo,
        const SendOptions &options,
        bool clearDraft = false,
        MsgId draftTopicRootId = MsgId(),
        PeerId draftMonoforumPeerId = PeerId());
    void sendFiles(
        History *history,
        std::vector<UploadSpec> files,
        const TextWithEntities &caption,
        ReplyTarget replyTo = {},
        // Reuses the nonces of an earlier attempt, one per file. The server is
        // idempotent per (user, chat, nonce), so a retry of a send whose reply
        // was lost returns the messages that already exist instead of posting
        // the album twice.
        const std::vector<QString> &reuseClientNonces = {},
        const SendOptions &options = {});
    // The link preview card as native media. Public so the composer's preview
    // adapter builds it exactly like an incoming message does - one converter
    // for one payload shape.
    [[nodiscard]] static std::optional<MTPMessageMedia> WebPageMedia(
        Main::Session *session,
        const QJsonObject &webPage);
    // The history of a FoxMes chat id, looked up by peer id directly rather
    // than through peerLoaded(): an event must not depend on whether the peer
    // finished loading. Null when the chat is unknown.
    [[nodiscard]] History *historyForChat(qint64 chatId) const;
    // Resolves the FoxMes chat id of a history, creating the direct chat when
    // it does not exist yet. Public so adapters can address a chat without
    // keeping a second copy of that lookup. An id of 0 means the chat could
    // not be resolved, and the caller has to fail visibly rather than wait.
    void resolveChatId(History *history, std::function<void(qint64)> done);
    // Builds the native message a queued reminder will become. Public because
    // the scheduled adapter renders the scheduled list from it, and there is
    // exactly one converter for one payload shape.
    [[nodiscard]] std::optional<MTPMessage> prepareReminder(
        History *history,
        const QJsonObject &reminder);
    bool retryFailedMessage(HistoryItem *item);
    // Re-issues sends that never reached the server, called when the live
    // connection is restored.
    void resendPendingSends();
    // Replays one send - a text, a single file, or a whole album - keeping the
    // nonces of the first attempt.
    void resendPendingGroup(const std::vector<qint64> &localIds);
    // Aborts the transfer behind a still-sending item, so cancelling actually
    // stops the upload instead of only removing the bubble.
    void cancelSend(HistoryItem *item);
    // Handles the response of a send the user cancelled after the commit
    // started: removes the pending entry and deletes the created message.
    // Returns true when the response must not be applied to the history.
    bool finishCancelledCommit(qint64 localId, const QJsonObject &message);
    void editText(
        HistoryItem *item,
        const TextWithEntities &text,
        Data::WebPageDraft webPage = {},
        std::function<void(QString)> done = {});
    void deleteMessages(
        History *history,
        const std::vector<int32_t> &ids,
        bool revoke,
        std::function<void()> done = {});
    void deleteHistory(History *history, bool deleteConversation);
    void deleteMessagesByDates(
        History *history,
        qint64 minDate,
        qint64 maxDate,
        std::function<void()> done = {});
    void markRead(History *history, qint64 messageId = 0);
    void readHistory(History *history, qint64 tillId, std::function<void()> done);
    void sendTyping(History *history);
    void setChatArchived(History *history, bool archived, std::function<void()> done = {});
    // Toggle chat-list pin through fxl-api. The optimistic local mutation is
    // done by the upstream caller before the hook; on failure this method
    // reverts it and reports success=false to the completion.
    void setChatPinned(
        History *history,
        bool pinned,
        std::function<void(bool success)> done = {});
    // Persists the full ordered list of pinned chats (drag-reorder).
    void savePinnedOrder(Data::Folder *folder);
    // Per-user "marked as unread" dialog flag; quiet reconcile on error.
    void setChatUnreadMark(History *history, bool marked);
    void saveNotificationSettings(PeerData *peer);
    // Per-user notification default for one peer type. Groups and channels
    // are outside the current product scope and are dropped here rather than
    // in the upstream queue, so the decision stays in one place.
    void saveDefaultNotifySettings(Data::DefaultNotify type);
    void react(HistoryItem *item, const QString &emoji);
    void setReactions(HistoryItem *item, const QStringList &emojis);
    // Dispatches the coalesced desired set for a message (one PUT in flight);
    // called again from the completion when the desired set changed meanwhile.
    void dispatchReactionReplace(History *history, qint64 messageId);
    // One page of found messages. total is the size of the whole result set,
    // which upstream reports as the search total: a per-page number there
    // ends the search after the first page.
    void searchMessages(
        History *history,
        const QString &query,
        qint64 before,
        int limit,
        std::function<void(std::vector<int32_t>, bool hasMore, int total)> done);
    // One page of the chat-list search: found messages of every chat, or of
    // one chat when a history is given. Messages are returned prepared rather than
    // applied, because the caller feeds them to the upstream response handler
    // that adds them itself.
    struct SearchPage {
        QVector<MTPMessage> messages;
        int total = 0;
        qint64 nextBefore = 0;
        bool hasMore = false;
    };
    // inHistory is null for a search across every chat.
    void searchAllChats(
        const QString &query,
        History *inHistory,
        qint64 before,
        int limit,
        std::function<void(SearchPage)> done);
    // One page of a global media list: the same lists across every chat.
    // Items are applied, because the list renders them from Data::Session.
    struct GlobalMediaPage {
        std::vector<FullMsgId> ids;
        int total = 0;
        qint64 nextBefore = 0;
        bool hasMore = false;
    };
    void requestGlobalMedia(
        const QString &kind,
        const QString &query,
        qint64 before,
        int limit,
        std::function<void(GlobalMediaPage)> done);
    // One page of a shared media list. The anchor is a position, and both
    // edges report their own exhaustion, exactly as a history page does.
    struct MediaPage {
        std::vector<int32_t> ids;
        bool hasMoreBefore = false;
        bool hasMoreAfter = false;
        int total = 0;
    };
    void requestChatMedia(
        History *history,
        const QString &kind,
        const QString &query,
        qint64 before,
        qint64 after,
        qint64 around,
        int limit,
        std::function<void(MediaPage)> done);
    void forwardMessages(
        History *source,
        History *target,
        const std::vector<int32_t> &ids,
        std::function<void(QString error)> done = {});
    void pinMessage(History *history, MsgId messageId, bool forEveryone, std::function<void(QString error)> done = {});
    void unpinMessage(History *history, MsgId messageId, std::function<void(QString error)> done = {});
    void unpinAllMessages(History *history, std::function<void(QString error)> done = {});
    // Serves ApiWrap::requestSharedMedia for SharedMediaType::Pinned: upstream
    // has no transport under the bridge, so the pinned slice comes from here.
    void requestPinnedMessages(History *history);
    void updateProfile(
        const QString &displayName,
        std::function<void(QJsonObject, QString)> done);
	[[nodiscard]] LiveUpdatesStatus liveUpdatesStatus() const;
	[[nodiscard]] rpl::producer<LiveUpdatesStatus> liveUpdatesStatusValue() const;
	void restartLiveUpdates();
	void stopLiveUpdates();
	void trackWindow(Window::SessionController *controller);
	void requestMessageData(
		PeerData *peer,
		MsgId messageId,
		std::function<void()> done);

	// One participant of a chat whose server read watermark already covers a
	// message, with the moment fxl-api recorded that read.
	struct ChatReader {
		qint64 userId = 0;
		TimeId date = 0;
	};
	// Readers other than me whose watermark covers messageId, newest read
	// first. Empty when the chat is unknown or nobody read that far.
	[[nodiscard]] std::vector<ChatReader> readersThrough(
		History *history,
		qint64 messageId) const;

private:
    [[nodiscard]] ApiClient &client() const;
    // Formatting of an incoming message: entities with UTF-16 offsets map
    // onto the native MTP entity types one to one.
    [[nodiscard]] static MTPVector<MTPMessageEntity> renderMessageEntities(
        const QJsonObject &message);
    // The outgoing direction of the same mapping. The text is needed because
    // a bare url entity carries no target of its own: the target is the text
    // it covers. The web page draft is what the composer's preview settings
    // produced; it rides on the link entity it belongs to, because that is the
    // link mark the server hangs the choice on.
    [[nodiscard]] static QJsonArray entitiesToJson(
        const EntitiesInText &entities,
        const QString &text,
        const Data::WebPageDraft &webPage = {});
    PeerData *peerForChat(const QJsonObject &chat);
    void applyChatConfig(PeerData *peer, const QJsonObject &chat);
    void applyChats(const QJsonDocument &doc);
    void rebuildPinnedOrder();
    void applyChatSettingsPatch(const QJsonObject &data);
    void loadCachedChats();
    void finishInitialLoadIfReady();
    void ensureUser(const QJsonObject &user, bool contact = false);
    HistoryItem *applyMessage(
        History *history,
        const QJsonObject &message,
        bool replaceExisting);
    HistoryItem *applyMessage(
        History *history,
        const QJsonObject &message,
        bool replaceExisting,
        NewMessageType type,
        qint64 pendingLocalIdHint = 0);
	HistoryItem *applyDependencyMessage(
		History *history,
		const QJsonObject &message);

    // Converts a server message JSON into an MTP message without touching
    // the history blocks. Used by applyMessage() for single events and by
    // loadHistoryPage() to build slices for addOlderSlice/addNewerSlice.
    struct PreparedMessage {
        MTPMessage mtp;
        MsgId messageId;
        qint64 senderId = 0;
    };
    [[nodiscard]] std::optional<PreparedMessage> prepareMessage(
        History *history,
        const QJsonObject &message,
        const LocalAttachment &local = {},
        // A reminder is not a message: it lives in its own id space, so its
        // id must never be written into the message revision cache - a
        // reminder and a message in the same chat can carry the same number.
        // Its date is the moment it will be delivered, not the moment it was
        // composed, because that is the date the scheduled list shows.
        bool reminder = false);

    void applyReminderResponse(
        History *history,
        const QJsonDocument &doc,
        const QString &error,
        int status);
    void clearCloudDraftFor(qint64 chatId);
    void dropOptimisticItems(
        History *history,
        const std::vector<qint64> &localIds);

    void applyMessageReactions(HistoryItem *item, const QJsonObject &message);
    void reloadMessageReactions(History *history, qint64 messageId);
    void applyPeerNotifySettings(
        PeerData *peer,
        qint64 muteUntil,
        bool showPreviews,
        bool soundNone);
    // Publishes the per-user defaults into the native model for all three
    // peer types. Must run before the first chat is applied: an unknown
    // default makes every unmuted peer read as muted.
    void applyDefaultNotifySettings(qint64 muteUntil, bool soundNone);
    void loadDefaultNotifySettings();
    void applyDefaultNotifySettingsPayload(const QJsonObject &settings);
    void applyNotificationSettings(
        PeerData *peer,
        qint64 chatId,
        const QJsonObject &notifications);
    void applyPinnedState(
        History *history,
        qint64 chatId,
        const QJsonObject &state);
    void syncPinnedMessages(History *history, qint64 chatId);
    void queueDelivered(qint64 chatId, qint64 messageId);
    void flushDelivered();
    void removeMessage(qint64 chatId, qint64 messageId);
    // Same, for callers that already hold the History: a confirmed deletion
    // must not depend on the chat->peer lookup succeeding.
    void removeMessageFrom(History *history, qint64 chatId, qint64 messageId);
    void removeHistoryThrough(
        qint64 chatId,
        qint64 throughMessageId,
        const QJsonArray &skippedPinnedIds);
    void handleEvent(const QJsonObject &event);
	void applyReadState(
		qint64 chatId,
		qint64 readerId,
		qint64 readThroughId,
		TimeId readAt,
		int unreadCount,
		qint64 stateRevision);
	void applyReceiptSnapshot(History *history, qint64 chatId, const QJsonObject &receiptState);
	void loadReadJournal();
	void persistReadJournal() const;
	void enqueueRead(qint64 chatId, qint64 desired, std::function<void()> done = {});
	void sendJournalRead(qint64 chatId);
	void scheduleReadRetry();
	void flushReadJournal();
	void pauseReadJournalForUnauthorized();
	void forceLiveUpdatesRestart();
	void resyncAfterGap(qint64 resumeFrom, qint64 observedSeq);
	void onWebSocketMessage(const QString &message);
	void refreshReactionsCatalog();
	void scheduleReactionsRefresh();
	void updatePresence();
	void applyPresence(const QJsonObject &data);
    void ensureChat(History *history, std::function<void(qint64)> done);
    void loadHistoryPage(
        History *history,
        qint64 aroundId,
        Data::LoadDirection direction,
        HistoryLoaded done = {});
    [[nodiscard]] qint64 chatIdFor(History *history) const;
    [[nodiscard]] History *historyForChatId(qint64 chatId) const;
    [[nodiscard]] PeerId peerForChatId(qint64 chatId) const;
    [[nodiscard]] static int32_t unixTime(const QString &value);
    [[nodiscard]] static QString renderMessageText(const QJsonObject &message);
    [[nodiscard]] HistoryItem *createPendingTextMessage(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        ReplyTarget replyTo,
        MsgId localMessageId);
    // One attachment is one message, so every file of a send gets its own
    // optimistic element. They share a client-side groupedId: a group can only
    // be set while the item is created, and it is never renumbered when the
    // sent messages come back - that is how upstream draws an album grid while
    // it is still uploading.
    // keepMedia comes back holding the document's media view when the send
    // has nothing but bytes in memory - see the body for why it has to be
    // created there and kept alive by the caller.
    [[nodiscard]] HistoryItem *createPendingFileMessage(
        History *history,
        const UploadSpec &file,
        const TextWithEntities &caption,
        ReplyTarget replyTo,
        const LocalAttachment &local,
        uint64 groupedId,
        std::shared_ptr<Data::DocumentMedia> &keepMedia);
    void rememberPendingSend(
        not_null<HistoryItem*> item,
        QString clientNonce,
        ReplyTarget replyTo,
        QString text,
        EntitiesInText entities,
        Data::WebPageDraft webPage,
        TextWithEntities caption,
        std::vector<UploadSpec> files,
        LocalAttachment local = {},
        bool clearDraft = false,
        MsgId draftTopicRootId = MsgId(),
        PeerId draftMonoforumPeerId = PeerId());
    // The HTTP status decides whether the send may be replayed on reconnect:
    // see SendMayBeRetried() in native_bridge.cpp.
    void failPendingSend(qint64 localId, int status);
    void clearPendingSend(qint64 localId);
    void finishPendingDraftSave(qint64 localId, TimeId savedAt);

    Main::Session *_session = nullptr;
    struct PendingSendRequest {
        History *history = nullptr;
        QString clientNonce;
        QString text;
        // Kept so retrying a failed send does not silently drop formatting.
        EntitiesInText entities;
        // The link preview choice of the first attempt, kept for the same
        // reason: a retry must not quietly turn the card back on.
        Data::WebPageDraft webPage;
        TextWithEntities caption;
        ReplyTarget replyTo;
        MsgId draftTopicRootId;
        PeerId draftMonoforumPeerId;
        bool draftSaving = false;
        std::vector<UploadSpec> files;
        // Album membership of a file send. Every element of one album shares
        // it, and a replay has to rebuild the whole group in a single send:
        // element by element the group would arrive as several one-file
        // albums, which is exactly what a grouped_id exists to prevent.
        uint64 groupedId = 0;
        LocalAttachment localAttachment;
        // Keeps the optimistic attachment's bytes reachable. A send whose
        // content never touched the disk - a recorded voice message or round
        // video, a picture pasted from the clipboard - has no path and no url
        // until the server answers, and DocumentData::setDataAndCache() can
        // only hand the bytes to an active media view, which does not exist
        // yet when the item is built. Holding one here is what creates it, so
        // the bubble is playable at once instead of being a dead document
        // (DocumentData::isNull() answers true without bytes, path or url, and
        // a null document is given no click handler at all).
        std::shared_ptr<Data::DocumentMedia> localMedia;
        std::function<void()> cancelUpload;
        // A request of this send - an upload, the commit - is out right now.
        // An upload can run for minutes, and a reconnect in the middle of one
        // must not start the whole chain a second time in parallel.
        bool inFlight = false;
        // Attachments are uploaded first and only then committed by the final
        // POST. Cancelling is free while uploading; once the POST is out the
        // server will create the message no matter what the client does.
        bool committing = false;
        // The user cancelled after the commit started: the message that comes
        // back has to be deleted on the server instead of being displayed.
        bool cancelledAfterCommit = false;
    };
    // Whether this send may be replayed on reconnect at all.
    [[nodiscard]] bool pendingSendReplayable(
        const PendingSendRequest &request) const;

    // Automatic replays already spent, per client_nonce: the counter outlives
    // the pending entry a replay replaces, and dies with the entry once the
    // send finally lands.
    std::unordered_map<QString, int> _sendReplays;
    std::unordered_map<uint64_t, qint64> _chatByPeer;
    std::unordered_map<qint64, uint64_t> _peerByChat;
    // Canonical pinned order from the server: chatId -> dense rank (>0),
    // only currently pinned chats are present.
    std::unordered_map<qint64, qint64> _pinnedRanks;
    std::unordered_set<MessageKey, MessageKeyHash> _seenMessages;
	std::unordered_map<qint64, std::vector<std::function<void()>>>
		_messageDataCallbacks;
	std::unordered_set<qint64> _loadedChats;
	// Chats whose pinned list was fetched through the bridge.
	std::unordered_set<qint64> _pinnedSyncedChats;
	// Last applied pinned list per chat, ascending. Kept so a new state can
	// clear the shared media entries of messages that are no longer pinned:
	// adding a slice never drops ids it does not mention.
	std::unordered_map<qint64, std::vector<qint64>> _pinnedIds;
	// Monotonic per-chat pin revision from the server. A payload carrying a
	// lower one is stale and is dropped whole.
	std::unordered_map<qint64, qint64> _pinRevisions;
	// Chats whose real tail page was fetched through the bridge, so the
	// loadedAtBottom flag of their History reflects actual data.
	std::unordered_set<qint64> _bottomLoadedChats;
	// In-flight history pages keyed by their request identity; every queued
	// completion is invoked exactly once when the response arrives.
	std::unordered_map<HistoryLoadKey, std::vector<std::function<void()>>, HistoryLoadKeyHash>
		_loadingChats;
	std::unordered_map<qint64, qint64> _nextHistoryBefore;
	std::unordered_map<uint64_t, std::vector<std::function<void(qint64)>>> _pendingChatCallbacks;
	std::unordered_map<qint64, PendingSendRequest> _pendingSends;
	std::unordered_map<QString, qint64> _pendingSendNonceToLocalId;
	std::unordered_map<qint64, QString> _avatarIds;
	std::unordered_map<qint64, std::unordered_map<qint64, qint64>>
		_presenceObservedAt;
	std::unordered_set<Window::SessionController*> _trackedWindows;
	qint64 _presenceChatId = 0;
	std::unordered_set<qint64> _pendingEdits;
	std::unordered_set<qint64> _pendingDeletes;
	// Incoming messages attached to a history but not yet acknowledged as
	// delivered, grouped by chat and flushed by _deliveredTimer.
	std::unordered_map<qint64, QList<qint64>> _pendingDelivered;
	QTimer _deliveredTimer;
	std::unordered_map<MessageKey, qint64, MessageKeyHash> _messageRevisions;
	struct AppliedReadState {
		qint64 readThroughId = 0;
		qint64 stateRevision = 0;
		// read_at as served by fxl-api; 0 when the server did not send one.
		TimeId readAt = 0;
	};
	std::unordered_map<MessageKey, AppliedReadState, MessageKeyHash>
		_appliedReadStates;
	struct ReadJournalEntry {
		qint64 desired = 0;
		qint64 retryAtMs = 0;
		int backoffSeconds = 1;
		bool inFlight = false;
		std::vector<std::function<void()>> completions;
	};
	std::unordered_map<qint64, ReadJournalEntry> _readJournal;
	QTimer _readRetryTimer;
	bool _readJournalPaused = false;
	std::unordered_map<uint64_t, uint64_t> _draftSaveGenerations;
	// Last server-accepted draft revision per chat: base_revision for the
	// next save and clear_draft_revision for sends.
	std::unordered_map<qint64, qint64> _draftRevisionByChat;
	// Reaction replace serialization: one in-flight PUT per message, new
	// clicks coalesce into the desired set; the canonical revision observed
	// from the last response guards the next attempt.
	struct ReactionReplaceState {
		QStringList desired;
		bool inFlight = false;
		// Zero is the canonical revision of a message nobody ever reacted
		// to, so it is also the right guard for the first replace on one.
		qint64 revision = 0;
	};
	std::unordered_map<qint64, ReactionReplaceState> _reactionReplace;
	// Last server-accepted notification settings per chat: the rollback
	// source for failed PUTs and the show_previews source of truth (the
	// native model does not expose it back).
	struct NotificationSnapshot {
		qint64 muteUntil = 0;
		bool showPreviews = true;
		// Native sound is a separate per-peer value from mute: a chat can be
		// unmuted and silent at once, so it round-trips on its own.
		bool soundNone = false;
		// Monotonic per-(chat,user) settings_revision this snapshot was
		// stored under. A patch older than it is a replayed or reordered
		// event and must not roll the settings back.
		qint64 revision = 0;
	};
	std::unordered_map<qint64, NotificationSnapshot> _notificationByChat;
	// Last server-accepted per-user notification defaults, mirrored from the
	// cache at startup so the setting survives a restart, plus the revision
	// that guards against a replayed or reordered settings.updated event.
	qint64 _defaultNotifyMuteUntil = 0;
	bool _defaultNotifySoundNone = false;
	qint64 _defaultNotifyRevision = 0;
	qint64 _eventSeq = 0;
	// Replay point of the last gap resync. A gap that asks to resume from the
	// very same place twice is a server/client disagreement, not a real gap:
	// resyncing again would only reconnect forever.
	qint64 _lastGapResumeFrom = 0;
	bool _contactsDone = false;
	bool _chatsDone = false;
	bool _reactionsRefreshScheduled = false;
	std::unique_ptr<LiveUpdatesConnection> _liveUpdates;

};

} // namespace CustomBackend
