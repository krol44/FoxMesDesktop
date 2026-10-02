/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "data/data_msg_id.h"
#include "mtproto/core_types.h"
#include "ui/text/text_entity.h"

#include <QtCore/QJsonObject>

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
}

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
}
}

namespace CustomBackend {
class NativeBridge;
struct UploadSpec;
}

namespace CustomBackend::TopicChannels {


inline constexpr auto kDiscussionIdShift = qint64(1) << 40;

[[nodiscard]] bool IsTopicChannel(const QJsonObject &chat);
[[nodiscard]] bool IsDiscussion(const QJsonObject &chat);
[[nodiscard]] bool IsTopicChannelPeer(not_null<PeerData*> peer);
[[nodiscard]] bool IsDiscussionPeer(not_null<PeerData*> peer);

[[nodiscard]] qint64 DiscussionId(qint64 channelId);
[[nodiscard]] qint64 ChannelOfDiscussion(qint64 discussionId);

[[nodiscard]] QJsonObject WithDiscussion(const QJsonObject &chat);
[[nodiscard]] QJsonObject DiscussionChat(const QJsonObject &chat);

void RedirectDiscussion(PeerId &peerId, MsgId &showAtMsgId);

[[nodiscard]] MTPRichMessage RichMessage(
	not_null<Main::Session*> session,
	const QJsonObject &article);
[[nodiscard]] MTPMessageReplies Replies(
	const QJsonObject &replies,
	qint64 channelId);

[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

[[nodiscard]] qint64 UploadTopicOf(
	not_null<History*> history,
	MsgId replyTo,
	MsgId rootId = MsgId());
[[nodiscard]] QJsonObject CommentBody(
	not_null<NativeBridge*> bridge,
	not_null<History*> history,
	const TextWithEntities &text);
void PostThreadComment(
	not_null<NativeBridge*> bridge,
	not_null<History*> history,
	MsgId replyTo,
	MsgId rootId,
	QJsonObject body,
	const QString &operationId,
	Fn<void(QString error, int status)> done);
[[nodiscard]] QString FailureText(const QString &error);
void EditComment(
	not_null<NativeBridge*> bridge,
	not_null<HistoryItem*> item,
	const TextWithEntities &text,
	Fn<void(QString)> done);
void SetCommentReactions(
	not_null<NativeBridge*> bridge,
	not_null<HistoryItem*> item,
	const std::vector<DocumentId> &emojiIds);

struct GifSource {
	qint64 chatId = 0;
	qint64 messageId = 0;
	qint64 threadRootId = 0;

	[[nodiscard]] explicit operator bool() const {
		return chatId != 0;
	}
};
[[nodiscard]] GifSource GifSourceOf(not_null<HistoryItem*> item);

[[nodiscard]] QJsonObject KeepOwnReactions(
	HistoryItem *item,
	QJsonObject message);

void ApplyDiscussionRead(
	not_null<NativeBridge*> bridge,
	const QJsonObject &data);

void ApplyCommentEvent(
	not_null<NativeBridge*> bridge,
	const QJsonObject &data);

}
