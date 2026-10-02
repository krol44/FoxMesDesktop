#pragma once

#include "api/api_common.h"
#include "custom_backend/api_client.h"
#include "custom_backend/live_updates_connection.h"
#include "data/data_drafts.h"
#include "data/data_messages.h"
#include "mtproto/mtproto_response.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChannelData;
class History;
class HistoryItem;
class PeerData;
class QImage;
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

struct HistoryLoadKey {
	qint64 chatId = 0;
	qint64 aroundId = 0;
	Data::LoadDirection direction = Data::LoadDirection::Before;
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
    bool forceFile = false;
    QString kind;
    qint64 durationMs = 0;
    QString waveform;
    QString performer;
    QString title;
    QByteArray cover;
    bool spoiler = false;
};

struct LocalAttachment {
    QByteArray bytes;
    QString path;
    bool forceFile = false;
};

[[nodiscard]] SendOptions SendOptionsFrom(
    const Api::SendOptions &options);

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
    using HistoryFailed = std::function<void(const MTP::Error &)>;
    void loadHistory(
        History *history,
        qint64 aroundId = 0,
        Data::LoadDirection direction = Data::LoadDirection::Before,
        HistoryLoaded done = {},
        HistoryFailed failed = {});
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
        const QString &reuseClientNonce = QString(),
        const SendOptions &options = {});
    void scheduleText(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        ReplyTarget replyTo,
        const SendOptions &options,
        bool clearDraft = false,
        MsgId draftTopicRootId = MsgId(),
        PeerId draftMonoforumPeerId = PeerId());
    void sendComment(
        History *history,
        const TextWithEntities &text,
        ReplyTarget replyTo,
        MsgId rootId,
        std::optional<MsgId> localMessageId,
        const QString &reuseClientNonce = QString());
    void finishCommentSend(
        History *history,
        const std::vector<qint64> &localIds,
        const QString &error,
        int status);
    void sendFiles(
        History *history,
        std::vector<UploadSpec> files,
        const TextWithEntities &caption,
        ReplyTarget replyTo = {},
        const std::vector<QString> &reuseClientNonces = {},
        const SendOptions &options = {});
    [[nodiscard]] static std::optional<MTPMessageMedia> WebPageMedia(
        Main::Session *session,
        const QJsonObject &webPage);
    [[nodiscard]] History *historyForChat(qint64 chatId) const;
    [[nodiscard]] bool hasChat(PeerId peerId) const;
    void resolveChatId(History *history, std::function<void(qint64)> done);
    [[nodiscard]] std::optional<MTPMessage> prepareReminder(
        History *history,
        const QJsonObject &reminder);
    bool retryFailedMessage(HistoryItem *item);
    void resendPendingSends();
    void resendPendingGroup(const std::vector<qint64> &localIds);
    void cancelSend(HistoryItem *item);
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
    void setChatPinned(
        History *history,
        bool pinned,
        std::function<void(bool success)> done = {});
    void savePinnedOrder(Data::Folder *folder);
    void setChatUnreadMark(History *history, bool marked);
    void saveNotificationSettings(PeerData *peer);
    void saveDefaultNotifySettings(Data::DefaultNotify type);
    void setReactions(
        HistoryItem *item,
        const std::vector<DocumentId> &emojiIds);
    void dispatchReactionReplace(History *history, qint64 messageId);
    void searchMessages(
        History *history,
        const QString &query,
        qint64 before,
        int limit,
        std::function<void(std::vector<int32_t>, bool hasMore, int total)> done);
    struct SearchPage {
        QVector<MTPMessage> messages;
        int total = 0;
        qint64 nextBefore = 0;
        bool hasMore = false;
    };
    void searchAllChats(
        const QString &query,
        History *inHistory,
        qint64 before,
        int limit,
        std::function<void(SearchPage)> done);
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
    struct MediaPage {
        std::vector<int32_t> ids;
        bool failed = false;
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
        MsgId threadRootId,
        bool dropAuthor,
        std::optional<int> videoTimestamp,
        std::function<void(QString error)> done = {});
    void pinMessage(History *history, MsgId messageId, bool forEveryone, std::function<void(QString error)> done = {});
    void unpinMessage(History *history, MsgId messageId, std::function<void(QString error)> done = {});
    void unpinAllMessages(History *history, std::function<void(QString error)> done = {});
    void requestPinnedMessages(History *history);
    void updateProfile(
        const QString &displayName,
        std::function<void(QJsonObject, QString)> done);
	[[nodiscard]] LiveUpdatesStatus liveUpdatesStatus() const;
	[[nodiscard]] rpl::producer<LiveUpdatesStatus> liveUpdatesStatusValue() const;
	void restartLiveUpdates();
	void stopLiveUpdates();
	[[nodiscard]] bool ephemeralMediaSupported() const {
		return _ephemeralMediaSupported;
	}
	void trackWindow(Window::SessionController *controller);
	void requestMessageData(
		PeerData *peer,
		MsgId messageId,
		std::function<void()> done);

	struct ChatReader {
		qint64 userId = 0;
		TimeId date = 0;
	};
	[[nodiscard]] std::vector<ChatReader> readersThrough(
		History *history,
		qint64 messageId) const;
	void ensureUser(const QJsonObject &user, bool contact = false);

	PeerId ensureQuotedChannel(const QJsonObject &channel);
	[[nodiscard]] bool isOwnMessage(
		not_null<History*> history,
		const QJsonObject &message) const;
	ChannelData *applyCommunityChat(
		const QJsonObject &chat,
		bool member = true);
	void reapplyCommunityChat(qint64 chatId);
	void applyCommunityProfile(const QJsonObject &profile);
	void uploadCommunityPhoto(
		ChannelData *channel,
		QImage &&image,
		std::function<void()> done,
		QByteArray video = QByteArray(),
		double videoStartTs = 0.,
		std::function<void()> fail = nullptr);
	[[nodiscard]] QJsonObject communityChat(qint64 chatId) const;

	[[nodiscard]] Main::Session &session() const {
		return *_session;
	}
	[[nodiscard]] static MTPMessageMedia AttachmentMedia(
		not_null<Main::Session*> session,
		const QJsonObject &attachment);
	[[nodiscard]] std::optional<MTPMessage> prepareAnswerMessage(
		History *history,
		const QJsonObject &message);
	HistoryItem *applyThreadMessage(
		History *history,
		const QJsonObject &message,
		NewMessageType type);
	[[nodiscard]] MTPEphemeralMessage welcomeMessage(
		const QJsonObject &welcome,
		bool isTemplate) const;
	[[nodiscard]] QJsonArray entitiesJson(
		const MTPVector<MTPMessageEntity> &entities,
		const QString &text) const;
	[[nodiscard]] MTPPeerNotifySettings communityNotifySettings(
		qint64 chatId,
		const QJsonObject &chat) const;
	[[nodiscard]] rpl::producer<bool> canCreateGroupsValue() const {
		return _canCreateGroups.value();
	}
	[[nodiscard]] rpl::producer<bool> canCreateChannelsValue() const {
		return _canCreateChannels.value();
	}
	[[nodiscard]] bool canCreateGroups() const {
		return _canCreateGroups.current();
	}
	[[nodiscard]] bool canCreateChannels() const {
		return _canCreateChannels.current();
	}

private:
	void applyCommunityPhoto(
		ChannelData *channel,
		const QString &url,
		const QString &videoUrl = QString(),
		int videoSize = 0,
		double videoStartTs = 0.);
	PhotoId applyRemoteChatPhoto(
		const QString &url,
		const QString &videoUrl,
		int videoSize,
		double videoStartTs);
	[[nodiscard]] MTPMessageAction communityPhotoAction(
		const QJsonObject &action);
	bool handleCommunityEvent(const QString &type, const QJsonObject &data);
    [[nodiscard]] ApiClient &client() const;
    [[nodiscard]] static MTPVector<MTPMessageEntity> renderMessageEntities(
        const QJsonObject &message);
    [[nodiscard]] QJsonArray entitiesToJson(
        const EntitiesInText &entities,
        const QString &text,
        const Data::WebPageDraft &webPage = {}) const;
    PeerData *peerForChat(const QJsonObject &chat);
    void applyChatConfig(PeerData *peer, const QJsonObject &chat);
    void applyChats(const QJsonDocument &doc);
    void removeChat(qint64 chatId);
    void rebuildPinnedOrder();
    void applyChatLookPatch(const QJsonObject &data);
    void applyChatSettingsPatch(const QJsonObject &data);
    void loadCachedChats();
    void finishInitialLoadIfReady();
    void loadHistoryOf(
        History *history,
        qint64 aroundId,
        Data::LoadDirection direction,
        HistoryLoaded done,
        HistoryFailed failed,
        bool chatsReloaded);
    void runAfterChatsReload(bool listLoaded);
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

    struct PreparedMessage {
        MTPMessage mtp;
        MsgId messageId;
        qint64 senderId = 0;
    };
    [[nodiscard]] std::optional<PreparedMessage> prepareMessage(
        History *history,
        const QJsonObject &message,
        const LocalAttachment &local = {},
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

    void applyMessagePayloadState(HistoryItem *item, const QJsonObject &message);
    void applyMessageReactions(HistoryItem *item, const QJsonObject &message);
public:
    void applyEphemeralViewed(
        HistoryItem *item,
        const QJsonObject &message);
    void applyEphemeralState(HistoryItem *item, const QJsonObject &message);
private:
    void reloadMessageReactions(History *history, qint64 messageId);
    void applyPeerNotifySettings(
        PeerData *peer,
        qint64 muteUntil,
        bool showPreviews,
        bool soundNone);
    void applyDefaultNotifySettings(
        Data::DefaultNotify type,
        qint64 muteUntil,
        bool soundNone);
    void loadDefaultNotifySettings();
    void refreshSelf();
    void offerTopicChannels();
    void startLiveUpdates();
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
	void refreshReactionUsage();
	void scheduleReactionUsageRefresh();
	void scheduleReactionsRefresh();
	void updatePresence();
	void applyPresence(const QJsonObject &data);
    void ensureChat(History *history, std::function<void(qint64)> done);
    void loadHistoryPage(
        History *history,
        qint64 aroundId,
        Data::LoadDirection direction,
        HistoryLoaded done = {});
    void deferMessageEvent(const QString &type, const QJsonObject &data);
    void drainDeferredMessageEvents();
    [[nodiscard]] std::optional<PreparedMessage> prepareServiceMessage(
        History *history,
        const QJsonObject &message,
        const QJsonObject &action,
        qint64 messageId,
        qint64 senderId);
    [[nodiscard]] std::optional<PreparedMessage> prepareCallMessage(
        History *history,
        const QJsonObject &message,
        const QJsonObject &call,
        qint64 messageId,
        qint64 senderId);
    [[nodiscard]] qint64 chatIdFor(History *history) const;
    [[nodiscard]] qint64 historyChatIdFor(History *history) const;
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
    [[nodiscard]] HistoryItem *createPendingFileMessage(
        History *history,
        const UploadSpec &file,
        const TextWithEntities &caption,
        ReplyTarget replyTo,
        const LocalAttachment &local,
        uint64 groupedId,
        int mediaTtlSeconds,
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
    void failPendingSend(qint64 localId, int status);
    void clearPendingSend(qint64 localId);
    void finishPendingDraftSave(qint64 localId, TimeId savedAt);

    Main::Session *_session = nullptr;
    struct PendingSendRequest {
        History *history = nullptr;
        QString clientNonce;
        QString text;
        EntitiesInText entities;
        Data::WebPageDraft webPage;
        TextWithEntities caption;
        ReplyTarget replyTo;
        MsgId draftTopicRootId;
        PeerId draftMonoforumPeerId;
        bool draftSaving = false;
        std::vector<UploadSpec> files;
        uint64 groupedId = 0;
        LocalAttachment localAttachment;
        std::shared_ptr<Data::DocumentMedia> localMedia;
        std::function<void()> cancelUpload;
        bool inFlight = false;
        bool committing = false;
        bool cancelledAfterCommit = false;
    };
    [[nodiscard]] bool pendingSendReplayable(
        const PendingSendRequest &request) const;

    std::unordered_map<QString, int> _sendReplays;
    uint64_t _chatsRequestGeneration = 0;
    std::unordered_map<uint64_t, qint64> _chatByPeer;
    std::unordered_map<qint64, uint64_t> _peerByChat;
    std::unordered_map<qint64, qint64> _pinnedRanks;
    std::unordered_set<MessageKey, MessageKeyHash> _seenMessages;
	std::unordered_map<qint64, std::vector<std::function<void()>>>
		_messageDataCallbacks;
	std::unordered_set<qint64> _loadedChats;
	std::unordered_set<qint64> _pinnedSyncedChats;
	std::unordered_map<qint64, std::vector<qint64>> _pinnedIds;
	std::unordered_map<qint64, qint64> _pinRevisions;
	std::unordered_set<qint64> _bottomLoadedChats;
	struct DeferredMessageEvent {
		QString type;
		QJsonObject data;
	};
	std::vector<DeferredMessageEvent> _deferredMessageEvents;
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
	std::unordered_map<Window::SessionController*, bool> _windowActive;
	qint64 _presenceChatId = 0;
	std::unordered_set<qint64> _pendingEdits;
	std::unordered_set<qint64> _pendingDeletes;
	std::unordered_map<qint64, QList<qint64>> _pendingDelivered;
	QTimer _deliveredTimer;
	std::unordered_map<MessageKey, qint64, MessageKeyHash> _messageRevisions;
	struct AppliedReadState {
		qint64 readThroughId = 0;
		qint64 stateRevision = 0;
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
	std::unordered_map<qint64, qint64> _draftRevisionByChat;
	struct ReactionReplaceState {
		std::vector<DocumentId> desired;
		bool inFlight = false;
		qint64 revision = 0;
	};
	std::unordered_map<qint64, ReactionReplaceState> _reactionReplace;
	struct NotificationSnapshot {
		qint64 muteUntil = 0;
		bool showPreviews = true;
		bool soundNone = false;
		qint64 revision = 0;
	};
	std::unordered_map<qint64, NotificationSnapshot> _notificationByChat;
	struct DefaultNotifyState {
		qint64 muteUntil = 0;
		bool soundNone = false;
		qint64 revision = 0;
	};
	std::array<DefaultNotifyState, 3> _defaultNotify;
	qint64 _eventSeq = 0;
	qint64 _lastGapResumeFrom = 0;
	bool _contactsDone = false;
	bool _chatsDone = false;
	std::vector<Fn<void(bool listLoaded)>> _afterChatsReload;
	bool _reactionsCatalogLoading = false;
	bool _reactionsRefreshScheduled = false;
	bool _reactionUsageRefreshScheduled = false;
	bool _ephemeralMediaSupported = false;
	rpl::variable<bool> _canCreateGroups = false;
	rpl::variable<bool> _canCreateChannels = false;
	bool _topicChannelsPrompt = false;
	bool _topicChannelsPromptOffered = false;
	std::unordered_map<qint64, QJsonObject> _communityChats;
	std::unique_ptr<LiveUpdatesConnection> _liveUpdates;

};

}
