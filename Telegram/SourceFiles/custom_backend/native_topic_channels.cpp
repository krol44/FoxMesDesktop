/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_topic_channels.h"

#include "api/api_text_entities.h"
#include "base/debug_log.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_community.h"
#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/topic_channel_links.h"
#include "data/data_channel.h"
#include "data/data_message_reaction_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_reply_markup.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "mtproto/details/mtproto_serialized_request.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrlQuery>

namespace CustomBackend::TopicChannels {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;
using Mtp::Answer;
using Mtp::Reader;

constexpr auto kSlowComment = crl::time(1500);

[[nodiscard]] qint64 Number(const QJsonObject &data, const char *key) {
	return data.value(QLatin1String(key)).toVariant().toLongLong();
}

[[nodiscard]] TimeId Time(const QString &value) {
	const auto parsed = QDateTime::fromString(value, Qt::ISODate);
	return parsed.isValid() ? TimeId(parsed.toSecsSinceEpoch()) : TimeId(0);
}

[[nodiscard]] QString PostPath(qint64 channelId, qint64 postId, const QString &tail) {
	return u"/chats/%1/posts/%2%3"_q.arg(channelId).arg(postId).arg(tail);
}

[[nodiscard]] base::flat_map<std::pair<qint64, qint64>, qint64> &PostByRoot() {
	static auto result = base::flat_map<std::pair<qint64, qint64>, qint64>();
	return result;
}

void RememberRoot(qint64 channelId, qint64 rootId, qint64 postId) {
	if (channelId > 0 && rootId > 0 && postId > 0) {
		PostByRoot()[{ DiscussionId(channelId), rootId }] = postId;
	}
}

[[nodiscard]] qint64 PostOfRoot(qint64 discussionId, qint64 rootId) {
	const auto &map = PostByRoot();
	const auto i = map.find({ discussionId, rootId });
	return (i != map.end()) ? i->second : 0;
}


[[nodiscard]] MTPRichText Text(const QJsonValue &value) {
	const auto node = value.toObject();
	const auto type = node.value("type").toString();
	const auto inner = [&] { return Text(node.value("inner")); };
	if (type == u"plain"_q) {
		return MTP_textPlain(MTP_string(node.value("text").toString()));
	} else if (type == u"concat"_q) {
		auto items = QVector<MTPRichText>();
		for (const auto &item : node.value("items").toArray()) {
			items.push_back(Text(item));
		}
		return MTP_textConcat(MTP_vector<MTPRichText>(items));
	} else if (type == u"bold"_q) {
		return MTP_textBold(inner());
	} else if (type == u"italic"_q) {
		return MTP_textItalic(inner());
	} else if (type == u"underline"_q) {
		return MTP_textUnderline(inner());
	} else if (type == u"strike"_q) {
		return MTP_textStrike(inner());
	} else if (type == u"fixed"_q) {
		return MTP_textFixed(inner());
	} else if (type == u"subscript"_q) {
		return MTP_textSubscript(inner());
	} else if (type == u"superscript"_q) {
		return MTP_textSuperscript(inner());
	} else if (type == u"marked"_q) {
		return MTP_textMarked(inner());
	} else if (type == u"spoiler"_q) {
		return MTP_textSpoiler(inner());
	} else if (type == u"url"_q) {
		return MTP_textUrl(
			inner(),
			MTP_string(node.value("url").toString()),
			MTP_long(0));
	} else if (type == u"emoji"_q) {
		return MTP_textCustomEmoji(
			MTP_long(Number(node, "emoji_id")),
			MTP_string(node.value("text").toString()));
	}
	return MTP_textEmpty();
}

[[nodiscard]] MTPPageCaption Caption(const QJsonValue &value) {
	return MTP_pageCaption(
		value.isObject() ? Text(value) : MTP_textEmpty(),
		MTP_textEmpty());
}

struct Media {
	QVector<MTPPhoto> photos;
	QVector<MTPDocument> documents;
	std::vector<uint64> ids;
};

[[nodiscard]] Media CollectMedia(
		not_null<Main::Session*> session,
		const QJsonArray &list) {
	auto result = Media();
	for (const auto &entry : list) {
		const auto media = NativeBridge::AttachmentMedia(
			session,
			entry.toObject());
		auto id = uint64(0);
		media.match([&](const MTPDmessageMediaPhoto &data) {
			if (const auto photo = data.vphoto()) {
				photo->match([&](const MTPDphoto &photo) {
					id = photo.vid().v;
				}, [](const auto &) {});
				result.photos.push_back(*photo);
			}
		}, [&](const MTPDmessageMediaDocument &data) {
			if (const auto document = data.vdocument()) {
				document->match([&](const MTPDdocument &document) {
					id = document.vid().v;
				}, [](const auto &) {});
				result.documents.push_back(*document);
			}
		}, [](const auto &) {});
		result.ids.push_back(id);
	}
	return result;
}

[[nodiscard]] QVector<MTPPageBlock> Blocks(
	const QJsonArray &list,
	const Media &media);

[[nodiscard]] uint64 MediaId(const QJsonObject &block, const Media &media) {
	const auto index = block.value("media").toInt(-1);
	return (index >= 0 && index < int(media.ids.size())) ? media.ids[index] : 0;
}

[[nodiscard]] MTPPageBlock Heading(int level, const MTPRichText &text) {
	switch (level) {
	case 2: return MTP_pageBlockHeading2(text);
	case 3: return MTP_pageBlockHeading3(text);
	case 4: return MTP_pageBlockHeading4(text);
	case 5: return MTP_pageBlockHeading5(text);
	case 6: return MTP_pageBlockHeading6(text);
	}
	return MTP_pageBlockHeading1(text);
}

[[nodiscard]] std::optional<MTPPageBlock> List(
		const QJsonObject &block,
		const Media &media) {
	const auto ordered = block.value("ordered").toBool();
	auto items = QVector<MTPPageListItem>();
	auto orderedItems = QVector<MTPPageListOrderedItem>();
	for (const auto &entry : block.value("items").toArray()) {
		const auto item = entry.toObject();
		const auto blocks = Blocks(item.value("blocks").toArray(), media);
		const auto checkbox = item.value("checkbox").toBool();
		const auto checked = item.value("checked").toBool();
		if (ordered) {
			using Flag = MTPDpageListOrderedItemBlocks::Flag;
			auto flags = MTPDpageListOrderedItemBlocks::Flags();
			if (checkbox) flags |= Flag::f_checkbox;
			if (checked) flags |= Flag::f_checked;
			orderedItems.push_back(MTP_pageListOrderedItemBlocks(
				MTP_flags(flags),
				MTPstring(),
				MTP_vector<MTPPageBlock>(blocks),
				MTPint(),
				MTPstring()));
		} else {
			using Flag = MTPDpageListItemBlocks::Flag;
			auto flags = MTPDpageListItemBlocks::Flags();
			if (checkbox) flags |= Flag::f_checkbox;
			if (checked) flags |= Flag::f_checked;
			items.push_back(MTP_pageListItemBlocks(
				MTP_flags(flags),
				MTP_vector<MTPPageBlock>(blocks)));
		}
	}
	if (!ordered) {
		return items.isEmpty()
			? std::nullopt
			: std::make_optional(MTP_pageBlockList(
				MTP_vector<MTPPageListItem>(items)));
	}
	if (orderedItems.isEmpty()) {
		return std::nullopt;
	}
	using Flag = MTPDpageBlockOrderedList::Flag;
	const auto start = block.value("start").toInt();
	return MTP_pageBlockOrderedList(
		MTP_flags(start > 1 ? Flag::f_start : Flag()),
		MTP_vector<MTPPageListOrderedItem>(orderedItems),
		MTP_int(start),
		MTPstring());
}

[[nodiscard]] MTPPageBlock Table(const QJsonObject &block) {
	auto rows = QVector<MTPPageTableRow>();
	for (const auto &entry : block.value("rows").toArray()) {
		auto cells = QVector<MTPPageTableCell>();
		for (const auto &value : entry.toObject().value("cells").toArray()) {
			const auto cell = value.toObject();
			using Flag = MTPDpageTableCell::Flag;
			auto flags = MTPDpageTableCell::Flags();
			if (cell.value("header").toBool()) flags |= Flag::f_header;
			const auto align = cell.value("align").toString();
			if (align == u"center"_q) flags |= Flag::f_align_center;
			if (align == u"right"_q) flags |= Flag::f_align_right;
			if (cell.value("text").isObject()) flags |= Flag::f_text;
			const auto colspan = cell.value("colspan").toInt();
			const auto rowspan = cell.value("rowspan").toInt();
			if (colspan > 1) flags |= Flag::f_colspan;
			if (rowspan > 1) flags |= Flag::f_rowspan;
			cells.push_back(MTP_pageTableCell(
				MTP_flags(flags),
				Text(cell.value("text")),
				MTP_int(colspan),
				MTP_int(rowspan)));
		}
		rows.push_back(MTP_pageTableRow(MTP_vector<MTPPageTableCell>(cells)));
	}
	return MTP_pageBlockTable(
		MTP_flags(MTPDpageBlockTable::Flag::f_bordered),
		MTP_textEmpty(),
		MTP_vector<MTPPageTableRow>(rows));
}

[[nodiscard]] std::optional<MTPPageBlock> Block(
		const QJsonObject &block,
		const Media &media) {
	const auto type = block.value("type").toString();
	const auto text = [&] { return Text(block.value("text")); };
	if (type == u"title"_q) {
		return MTP_pageBlockTitle(text());
	} else if (type == u"author_date"_q) {
		return MTP_pageBlockAuthorDate(
			Text(block.value("author")),
			MTP_int(block.value("date").toVariant().toLongLong()));
	} else if (type == u"paragraph"_q) {
		return MTP_pageBlockParagraph(text());
	} else if (type == u"heading"_q) {
		return Heading(block.value("level").toInt(), text());
	} else if (type == u"pre"_q) {
		return MTP_pageBlockPreformatted(
			text(),
			MTP_string(block.value("language").toString()));
	} else if (type == u"divider"_q) {
		return MTP_pageBlockDivider();
	} else if (type == u"list"_q) {
		return List(block, media);
	} else if (type == u"blockquote"_q) {
		return MTP_pageBlockBlockquoteBlocks(
			MTP_vector<MTPPageBlock>(Blocks(block.value("blocks").toArray(), media)),
			MTP_textEmpty());
	} else if (type == u"details"_q) {
		using Flag = MTPDpageBlockDetails::Flag;
		return MTP_pageBlockDetails(
			MTP_flags(block.value("open").toBool() ? Flag::f_open : Flag()),
			MTP_vector<MTPPageBlock>(Blocks(block.value("blocks").toArray(), media)),
			text());
	} else if (type == u"table"_q) {
		return Table(block);
	} else if (type == u"map"_q) {
		return MTP_pageBlockMap(
			MTP_geoPoint(
				MTP_flags(0),
				MTP_double(block.value("longitude").toDouble()),
				MTP_double(block.value("latitude").toDouble()),
				MTP_long(0),
				MTPint()),
			MTP_int(15),
			MTP_int(600),
			MTP_int(300),
			Caption(block.value("caption")));
	}
	const auto id = MediaId(block, media);
	if (!id) {
		return std::nullopt;
	}
	const auto caption = Caption(block.value("caption"));
	if (type == u"photo"_q) {
		using Flag = MTPDpageBlockPhoto::Flag;
		const auto url = block.value("url").toString();
		auto flags = MTPDpageBlockPhoto::Flags();
		if (!url.isEmpty()) flags |= Flag::f_url;
		if (block.value("spoiler").toBool()) flags |= Flag::f_spoiler;
		return MTP_pageBlockPhoto(
			MTP_flags(flags),
			MTP_long(id),
			caption,
			MTP_string(url),
			MTP_long(0));
	} else if (type == u"video"_q) {
		using Flag = MTPDpageBlockVideo::Flag;
		auto flags = MTPDpageBlockVideo::Flags();
		if (block.value("autoplay").toBool()) flags |= Flag::f_autoplay;
		if (block.value("loop").toBool()) flags |= Flag::f_loop;
		if (block.value("spoiler").toBool()) flags |= Flag::f_spoiler;
		return MTP_pageBlockVideo(MTP_flags(flags), MTP_long(id), caption);
	} else if (type == u"audio"_q) {
		return MTP_pageBlockAudio(MTP_long(id), caption);
	} else if (type == u"document"_q) {
		return MTP_pageBlockDocument(MTP_long(id), caption);
	}
	return std::nullopt;
}

QVector<MTPPageBlock> Blocks(const QJsonArray &list, const Media &media) {
	auto result = QVector<MTPPageBlock>();
	for (const auto &entry : list) {
		if (const auto block = Block(entry.toObject(), media)) {
			result.push_back(*block);
		}
	}
	return result;
}


struct Context {
	base::weak_ptr<Main::Session> session;
	ApiClient *client = nullptr;
	Answer answer;
	qint64 me = 0;
};

[[nodiscard]] NativeBridge *BridgeOf(const Context &context) {
	const auto session = context.session.get();
	return session ? BridgeFor(session) : nullptr;
}

[[nodiscard]] qint64 ChatId(const MTPInputPeer &peer) {
	return peer.match([](const MTPDinputPeerChannel &data) {
		return qint64(data.vchannel_id().v);
	}, [](const MTPDinputPeerChannelFromMessage &data) {
		return qint64(data.vchannel_id().v);
	}, [](const auto &) {
		return qint64(0);
	});
}

void Request(
		const Context &context,
		const QByteArray &method,
		const QString &path,
		const QJsonObject &body,
		Fn<void(QJsonObject)> done) {
	const auto answer = context.answer;
	context.client->communityRequest(method, path, body, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		if (!error.isEmpty()) {
			const auto type = Mtp::ErrorType(
				doc,
				error,
				status,
				u"MSG_ID_INVALID"_q,
				u"CHANNEL_PRIVATE"_q);
			LOG(("FoxMes TopicChannels: %1 %2 failed, status %3, error %4"
				).arg(QString::fromLatin1(method), path).arg(status).arg(type));
			answer.fail(type, (status >= 400 && status < 600) ? status : 400);
			return;
		}
		done(doc.object());
	});
}

[[nodiscard]] QVector<MTPChat> ThreadChats(
		const Context &context,
		not_null<NativeBridge*> bridge,
		qint64 channelId) {
	const auto chat = bridge->communityChat(channelId);
	if (chat.isEmpty()) {
		return {};
	}
	const auto discussion = DiscussionChat(chat);
	bridge->applyCommunityChat(discussion, false);
	const auto member = bridge->hasChat(peerFromChannel(ChannelId(channelId)));
	return {
		Community::Channel(WithDiscussion(chat), context.me, member),
		Community::Channel(discussion, context.me, false),
	};
}

[[nodiscard]] MTPMessage RootMessage(
		not_null<Main::Session*> session,
		const QJsonObject &post,
		qint64 channelId,
		qint64 rootId) {
	using Flag = MTPDmessage::Flag;
	using FwdFlag = MTPDmessageFwdHeader::Flag;
	const auto postId = Number(post, "id");
	const auto date = MTP_int(Time(post.value("created_at").toString()));
	const auto channel = MTP_peerChannel(MTP_long(channelId));
	const auto article = post.value("article").toObject();
	RememberSiteLink(
		session,
		FullMsgId(peerFromChannel(ChannelId(DiscussionId(channelId))), rootId),
		post.value("site_url").toString());
	auto flags = Flag::f_from_id | Flag::f_fwd_from | Flag();
	if (!article.isEmpty()) {
		flags |= Flag::f_rich_message;
	}
	return MTP_message(
		MTP_flags(flags),
		MTP_int(int(rootId)),
		channel,
		MTPint(),
		MTPstring(),
		MTP_peerChannel(MTP_long(DiscussionId(channelId))),
		MTPPeer(),
		MTP_messageFwdHeader(
			MTP_flags(FwdFlag::f_from_id
				| FwdFlag::f_channel_post
				| FwdFlag::f_saved_from_peer
				| FwdFlag::f_saved_from_msg_id),
			channel,
			MTPstring(),
			date,
			MTP_int(int(postId)),
			MTPstring(),
			channel,
			MTP_int(int(postId)),
			MTPPeer(),
			MTPstring(),
			MTPint(),
			MTPstring()),
		MTPlong(),
		MTPlong(),
		MTPPeer(),
		MTPMessageReplyHeader(),
		date,
		MTP_string(),
		MTP_messageMediaEmpty(),
		MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(),
		MTPint(),
		MTPint(),
		MTPMessageReplies(),
		MTPint(),
		MTPstring(),
		MTPlong(),
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(),
		MTPint(),
		MTPlong(),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTPint(),
		MTPstring(),
		article.isEmpty() ? MTPRichMessage() : RichMessage(session, article));
}

void ApplyUsers(not_null<NativeBridge*> bridge, const QJsonObject &data) {
	for (const auto &user : data.value("users").toArray()) {
		bridge->ensureUser(user.toObject());
	}
}

[[nodiscard]] QJsonObject InDiscussion(
		QJsonObject comment,
		qint64 channelId,
		qint64 rootId) {
	comment.insert("chat_id", DiscussionId(channelId));
	comment.insert("thread_root_id", rootId);
	return comment;
}

void GetRichMessage(const Context &context, Reader &reader) {
	const auto channelId = ChatId(reader.read<MTPInputPeer>());
	const auto id = reader.read<MTPint>().v;
	if (!reader.ok() || channelId <= 0 || id <= 0) {
		context.answer.fail(u"MSG_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "GET", u"/chats/%1/messages/%2"_q.arg(channelId).arg(id), {}, [=](
			QJsonObject message) {
		const auto bridge = BridgeOf(context);
		const auto session = context.session.get();
		if (!bridge || !session) {
			answer.fail(u"MSG_ID_INVALID"_q);
			return;
		}
		const auto history = session->data().history(
			peerFromChannel(ChannelId(channelId)));
		const auto prepared = bridge->prepareAnswerMessage(history, message);
		if (!prepared) {
			answer.fail(u"MSG_ID_INVALID"_q);
			return;
		}
		answer.done(MTP_messages_messages(
			MTP_vector<MTPMessage>(1, *prepared),
			MTPVector<MTPForumTopic>(),
			MTPVector<MTPChat>(),
			MTPVector<MTPUser>()));
	});
}

void GetDiscussionMessage(const Context &context, Reader &reader) {
	const auto channelId = ChatId(reader.read<MTPInputPeer>());
	const auto postId = reader.read<MTPint>().v;
	if (!reader.ok() || channelId <= 0 || postId <= 0) {
		context.answer.fail(u"MSG_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "GET", PostPath(channelId, postId, u"/discussion"_q), {}, [=](
			QJsonObject data) {
		const auto bridge = BridgeOf(context);
		const auto session = context.session.get();
		if (!bridge || !session) {
			answer.fail(u"CHANNEL_PRIVATE"_q);
			return;
		}
		const auto rootId = Number(data, "root_id");
		const auto maxId = MTP_int(data.value("max_id").toInt());
		const auto readMaxId = data.value("read_max_id").toInt();
		RememberRoot(channelId, rootId, postId);
		const auto chats = ThreadChats(context, bridge, channelId);
		if (chats.isEmpty()) {
			answer.fail(u"CHANNEL_PRIVATE"_q);
			return;
		}
		using Flag = MTPDmessages_discussionMessage::Flag;
		auto flags = Flag::f_max_id | Flag::f_read_outbox_max_id | Flag();
		if (readMaxId > 0) {
			flags |= Flag::f_read_inbox_max_id;
		}
		answer.done(MTP_messages_discussionMessage(
			MTP_flags(flags),
			MTP_vector<MTPMessage>(1, RootMessage(
				session,
				data.value("post").toObject(),
				channelId,
				rootId)),
			maxId,
			MTP_int(readMaxId),
			maxId,
			MTP_int(data.value("unread_count").toInt()),
			MTP_vector<MTPChat>(chats),
			MTPVector<MTPUser>()));
	});
}

void GetReplies(const Context &context, Reader &reader) {
	const auto discussionId = ChatId(reader.read<MTPInputPeer>());
	const auto rootId = reader.read<MTPint>().v;
	const auto offsetId = reader.read<MTPint>().v;
	reader.read<MTPint>();
	const auto addOffset = reader.read<MTPint>().v;
	const auto limit = reader.read<MTPint>().v;
	const auto channelId = ChannelOfDiscussion(discussionId);
	const auto postId = PostOfRoot(discussionId, rootId);
	if (!reader.ok() || !channelId || !postId) {
		context.answer.fail(u"MSG_ID_INVALID"_q);
		return;
	}
	auto query = QUrlQuery();
	query.addQueryItem(u"limit"_q, QString::number(std::max(limit, 1)));
	if (offsetId > 0 && addOffset >= 0) {
		query.addQueryItem(u"before"_q, QString::number(offsetId));
	} else if (offsetId > 0 && addOffset <= -limit) {
		query.addQueryItem(u"after"_q, QString::number(offsetId - 1));
	} else if (offsetId > 0) {
		query.addQueryItem(u"around"_q, QString::number(offsetId));
	}
	const auto answer = context.answer;
	const auto path = PostPath(channelId, postId, u"/comments?"_q)
		+ query.toString(QUrl::FullyEncoded);
	Request(context, "GET", path, {}, [=](QJsonObject data) {
		const auto bridge = BridgeOf(context);
		const auto session = context.session.get();
		if (!bridge || !session) {
			answer.fail(u"CHANNEL_PRIVATE"_q);
			return;
		}
		ApplyUsers(bridge, data);
		const auto chats = ThreadChats(context, bridge, channelId);
		const auto history = session->data().history(
			peerFromChannel(ChannelId(discussionId)));
		auto messages = QVector<MTPMessage>();
		const auto items = data.value("items").toArray();
		for (auto i = items.size(); i != 0;) {
			const auto prepared = bridge->prepareAnswerMessage(
				history,
				InDiscussion(items.at(--i).toObject(), channelId, rootId));
			if (prepared) {
				messages.push_back(*prepared);
			}
		}
		answer.done(MTP_messages_channelMessages(
			MTP_flags(0),
			MTP_int(0),
			MTP_int(data.value("count").toInt()),
			MTPint(),
			MTP_vector<MTPMessage>(messages),
			MTPVector<MTPForumTopic>(),
			MTP_vector<MTPChat>(chats),
			MTPVector<MTPUser>()));
	});
}

void ReadDiscussion(const Context &context, Reader &reader) {
	const auto discussionId = ChatId(reader.read<MTPInputPeer>());
	const auto rootId = reader.read<MTPint>().v;
	const auto readMaxId = reader.read<MTPint>().v;
	const auto channelId = ChannelOfDiscussion(discussionId);
	const auto postId = PostOfRoot(discussionId, rootId);
	if (!reader.ok() || channelId <= 0 || readMaxId <= 0) {
		context.answer.fail(u"MSG_ID_INVALID"_q);
		return;
	} else if (!postId) {
		context.answer.done(MTP_boolTrue());
		return;
	}
	const auto answer = context.answer;
	Request(
		context,
		"POST",
		PostPath(channelId, postId, u"/discussion/read"_q),
		QJsonObject{ { "read_max_id", readMaxId } },
		[=](QJsonObject) { answer.done(MTP_boolTrue()); });
}

void GetRecommendations(const Context &context, Reader &) {
	const auto answer = context.answer;
	context.client->communityRequest("GET", u"/channels/recommended"_q, {}, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		const auto bridge = BridgeOf(context);
		auto chats = QVector<MTPChat>();
		if (!bridge || !error.isEmpty()) {
			LOG(("FoxMes TopicChannels: recommendations failed, status %1, %2"
				).arg(status).arg(error));
			answer.done(MTP_messages_chats(MTP_vector<MTPChat>(chats)));
			return;
		}
		const auto data = doc.object();
		for (const auto &entry : data.value("chats").toArray()) {
			const auto chat = entry.toObject();
			bridge->applyCommunityChat(chat, false);
			chats.push_back(Community::Channel(
				WithDiscussion(chat),
				context.me,
				false));
		}
		answer.done(MTP_messages_chats(MTP_vector<MTPChat>(chats)));
	});
}

void GetDiscussionParticipants(const Context &context, Reader &) {
	context.answer.done(MTP_channels_channelParticipants(
		MTP_int(0),
		MTP_vector<MTPChannelParticipant>(),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>()));
}

void GetDiscussionParticipant(const Context &context, Reader &) {
	context.answer.fail(u"USER_NOT_PARTICIPANT"_q);
}

[[nodiscard]] bool AsksDiscussion(const SerializedRequest &request) {
	auto reader = Reader(request);
	const auto channel = reader.read<MTPInputChannel>();
	const auto channelId = channel.match([](const MTPDinputChannel &data) {
		return qint64(data.vchannel_id().v);
	}, [](const MTPDinputChannelFromMessage &data) {
		return qint64(data.vchannel_id().v);
	}, [](const MTPDinputChannelEmpty &) {
		return qint64(0);
	});
	return reader.ok() && (ChannelOfDiscussion(channelId) > 0);
}

using Handler = void(*)(const Context &, Reader &);

[[nodiscard]] Handler HandlerFor(const SerializedRequest &request) {
	switch (Mtp::RequestType(request)) {
	case mtpc_channels_getParticipants:
		return AsksDiscussion(request) ? GetDiscussionParticipants : nullptr;
	case mtpc_channels_getParticipant:
		return AsksDiscussion(request) ? GetDiscussionParticipant : nullptr;
	case mtpc_messages_getRichMessage: return GetRichMessage;
	case mtpc_messages_getDiscussionMessage: return GetDiscussionMessage;
	case mtpc_messages_getReplies: return GetReplies;
	case mtpc_messages_readDiscussion: return ReadDiscussion;
	case mtpc_channels_getChannelRecommendations: return GetRecommendations;
	}
	return nullptr;
}


struct Thread {
	qint64 channelId = 0;
	qint64 rootId = 0;
	qint64 postId = 0;

	[[nodiscard]] explicit operator bool() const {
		return channelId && rootId && postId;
	}
	[[nodiscard]] qint64 topicId() const {
		return (rootId - 1) / 2;
	}
};

[[nodiscard]] Thread ThreadOf(not_null<History*> history, MsgId rootId) {
	const auto discussionId = qint64(peerToChannel(history->peer->id).bare);
	const auto channelId = ChannelOfDiscussion(discussionId);
	return {
		.channelId = channelId,
		.rootId = rootId.bare,
		.postId = PostOfRoot(discussionId, rootId.bare),
	};
}

[[nodiscard]] MsgId RootOf(not_null<HistoryItem*> item) {
	if (const auto top = item->replyToTop()) {
		return top;
	} else if (const auto parent = item->replyToId()) {
		return parent;
	}
	return item->id;
}

HistoryItem *ApplyComment(
		not_null<NativeBridge*> bridge,
		not_null<History*> history,
		const Thread &thread,
		const QJsonObject &data,
		NewMessageType type) {
	ApplyUsers(bridge, data);
	return bridge->applyThreadMessage(
		history,
		InDiscussion(
			data.value("message").toObject(),
			thread.channelId,
			thread.rootId),
		type);
}

[[nodiscard]] Thread ThreadOfReply(
		not_null<History*> history,
		MsgId replyTo,
		MsgId rootId) {
	const auto root = rootId ? rootId : [&] {
		const auto parent = history->owner().message(history->peer, replyTo);
		return parent ? RootOf(parent) : replyTo;
	}();
	return ThreadOf(history, root);
}

[[nodiscard]] QJsonObject CommentText(
		not_null<NativeBridge*> bridge,
		not_null<History*> history,
		const TextWithEntities &text) {
	return QJsonObject{
		{ "text", text.text },
		{ "entities", bridge->entitiesJson(
			Api::EntitiesToMTP(
				&history->session(),
				text.entities,
				Api::ConvertOption::WithLocal),
			text.text) },
	};
}

[[nodiscard]] QString FailureTextOf(const QString &error) {
	if (error == u"STICKER_CHAT_ONLY"_q) {
		return u"This sticker is only in FoxMes: the site can't show it"_q;
	} else if (error == u"MEDIA_NOT_ON_SITE"_q) {
		return u"Voice and video messages can't be sent to the site's comments"_q;
	}
	return error;
}

void PostComment(
		not_null<NativeBridge*> bridge,
		not_null<History*> history,
		const Thread &thread,
		MsgId replyTo,
		QJsonObject body,
		Fn<void(QString error, int status)> done) {
	body.insert("reply_to_id", replyTo.bare ? replyTo.bare : thread.rootId);
	const auto session = &history->session();
	const auto weak = base::make_weak(session);
	const auto requested = crl::now();
	ClientFor(session).communityRequest(
		"POST",
		PostPath(thread.channelId, thread.postId, u"/comments"_q),
		body,
		[=](QJsonDocument doc, QString error, int status) {
			const auto strong = weak.get();
			const auto bridge = strong ? BridgeFor(strong) : nullptr;
			if (!bridge) {
				return;
			} else if (!error.isEmpty()) {
				const auto code = doc.object().value("code").toString();
				LOG(("FoxMes TopicChannels: comment failed, status %1, %2"
					).arg(status).arg(error));
				done(code.isEmpty() ? error : code, status);
				return;
			}
			const auto took = crl::now() - requested;
			if (took >= kSlowComment) {
				LOG(("FoxMes TopicChannels: comment took %1 ms").arg(took));
			}
			if (!ApplyComment(bridge, history, thread, doc.object(), NewMessageType::Unread)) {
				LOG(("FoxMes TopicChannels: the sent comment was not applied"));
			}
			done(QString(), status);
		});
}

} // namespace

bool IsTopicChannel(const QJsonObject &chat) {
	return Community::IsChannel(chat) && Number(chat, "category_id") > 0;
}

bool IsDiscussion(const QJsonObject &chat) {
	return Number(chat, "discussion_of") > 0;
}

bool IsTopicChannelPeer(not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	const auto bridge = channel ? BridgeFor(&peer->session()) : nullptr;
	if (!bridge) {
		return false;
	}
	const auto chat = bridge->communityChat(qint64(peerToChannel(channel->id).bare));
	return IsTopicChannel(chat) || IsDiscussion(chat);
}

bool IsDiscussionPeer(not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	return channel && ChannelOfDiscussion(qint64(peerToChannel(channel->id).bare)) > 0;
}

qint64 DiscussionId(qint64 channelId) {
	return channelId + kDiscussionIdShift;
}

qint64 ChannelOfDiscussion(qint64 discussionId) {
	return (discussionId > kDiscussionIdShift)
		? (discussionId - kDiscussionIdShift)
		: 0;
}

QJsonObject WithDiscussion(const QJsonObject &chat) {
	if (!IsTopicChannel(chat)) {
		return chat;
	}
	auto result = chat;
	result.insert("linked_chat_id", DiscussionId(Number(chat, "id")));
	return result;
}

QJsonObject DiscussionChat(const QJsonObject &chat) {
	const auto channelId = Number(chat, "id");
	return QJsonObject{
		{ "id", DiscussionId(channelId) },
		{ "type", u"group"_q },
		{ "title", chat.value("title") },
		{ "owner_id", chat.value("owner_id") },
		{ "photo_url", chat.value("photo_url") },
		{ "reactions", chat.value("reactions") },
		{ "created_at", chat.value("created_at") },
		{ "category_id", chat.value("category_id") },
		{ "discussion_of", channelId },
		{ "linked_chat_id", channelId },
	};
}

void RedirectDiscussion(PeerId &peerId, MsgId &showAtMsgId) {
	if (!peerIsChannel(peerId)) {
		return;
	}
	const auto channelId = ChannelOfDiscussion(
		qint64(peerToChannel(peerId).bare));
	if (channelId <= 0) {
		return;
	}
	peerId = peerFromChannel(ChannelId(channelId));
	showAtMsgId = ShowAtUnreadMsgId;
}

MTPRichMessage RichMessage(
		not_null<Main::Session*> session,
		const QJsonObject &article) {
	const auto media = CollectMedia(session, article.value("media").toArray());
	using Flag = MTPDrichMessage::Flag;
	return MTP_richMessage(
		MTP_flags(article.value("part").toBool() ? Flag::f_part : Flag()),
		MTP_vector<MTPPageBlock>(Blocks(article.value("blocks").toArray(), media)),
		MTP_vector<MTPPhoto>(media.photos),
		MTP_vector<MTPDocument>(media.documents));
}

MTPMessageReplies Replies(const QJsonObject &replies, qint64 channelId) {
	using Flag = MTPDmessageReplies::Flag;
	const auto maxId = replies.value("max_id").toInt();
	const auto readMaxId = replies.value("read_max_id").toInt();
	auto flags = Flag::f_comments | Flag::f_channel_id | Flag();
	if (maxId > 0) {
		flags |= Flag::f_max_id;
	}
	if (readMaxId > 0) {
		flags |= Flag::f_read_max_id;
	}
	return MTP_messageReplies(
		MTP_flags(flags),
		MTP_int(replies.value("count").toInt()),
		MTP_int(0),
		MTPVector<MTPPeer>(),
		MTP_long(DiscussionId(channelId)),
		MTP_int(maxId),
		MTP_int(readMaxId));
}

bool Intercepts(const SerializedRequest &request) {
	return HandlerFor(request) != nullptr;
}

void Intercept(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const SerializedRequest &request) {
	const auto handler = HandlerFor(request);
	const auto session = Mtp::SessionFor(instance);
	auto context = Context{
		.session = base::make_weak(session),
		.client = session ? &ClientFor(session) : nullptr,
		.answer = Answer(instance, requestId),
	};
	if (!handler || !context.client) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	context.me = context.client->meId();
	auto reader = Reader(request);
	handler(context, reader);
}

QString FailureText(const QString &error) {
	return FailureTextOf(error);
}

qint64 UploadTopicOf(
		not_null<History*> history,
		MsgId replyTo,
		MsgId rootId) {
	const auto thread = ThreadOfReply(history, replyTo, rootId);
	return thread ? thread.topicId() : 0;
}

QJsonObject CommentBody(
		not_null<NativeBridge*> bridge,
		not_null<History*> history,
		const TextWithEntities &text) {
	return CommentText(bridge, history, text);
}

void PostThreadComment(
		not_null<NativeBridge*> bridge,
		not_null<History*> history,
		MsgId replyTo,
		MsgId rootId,
		QJsonObject body,
		const QString &operationId,
		Fn<void(QString error, int status)> done) {
	const auto thread = ThreadOfReply(history, replyTo, rootId);
	if (!thread) {
		LOG(("FoxMes TopicChannels: no thread for a comment in %1, "
			"reply %2, root %3, found root %4, post %5"
			).arg(history->peer->id.value
			).arg(replyTo.bare
			).arg(rootId.bare
			).arg(thread.rootId
			).arg(thread.postId));
		done(u"MSG_ID_INVALID"_q, 400);
		return;
	}
	if (!operationId.isEmpty()) {
		body.insert("operation_id", operationId);
	}
	PostComment(bridge, history, thread, replyTo, std::move(body), std::move(done));
}

void EditComment(
		not_null<NativeBridge*> bridge,
		not_null<HistoryItem*> item,
		const TextWithEntities &text,
		Fn<void(QString)> done) {
	const auto history = item->history();
	const auto thread = ThreadOf(history, RootOf(item));
	if (!thread) {
		if (done) done(u"MESSAGE_ID_INVALID"_q);
		return;
	}
	const auto session = &history->session();
	const auto body = QJsonObject{
		{ "text", text.text },
		{ "entities", bridge->entitiesJson(
			Api::EntitiesToMTP(session, text.entities, Api::ConvertOption::WithLocal),
			text.text) },
	};
	const auto weak = base::make_weak(session);
	ClientFor(session).communityRequest(
		"PATCH",
		PostPath(thread.channelId, thread.postId, u"/comments/%1"_q.arg(item->id.bare)),
		body,
		[=](QJsonDocument doc, QString error, int) {
			const auto strong = weak.get();
			const auto bridge = strong ? BridgeFor(strong) : nullptr;
			if (!bridge || !error.isEmpty()) {
				const auto code = doc.object().value("code").toString();
				if (done) done(code.isEmpty() ? u"MESSAGE_EDIT_TIME_EXPIRED"_q : code);
				return;
			}
			ApplyComment(bridge, history, thread, doc.object(), NewMessageType::Existing);
			if (done) done(QString());
		});
}

void SetCommentReactions(
		not_null<NativeBridge*> bridge,
		not_null<HistoryItem*> item,
		const std::vector<DocumentId> &emojiIds) {
	const auto history = item->history();
	const auto thread = ThreadOf(history, RootOf(item));
	if (!thread) {
		return;
	}
	auto ids = QJsonArray();
	for (const auto id : emojiIds) {
		ids.push_back(qint64(id));
	}
	const auto session = &history->session();
	const auto weak = base::make_weak(session);
	ClientFor(session).communityRequest(
		"PUT",
		PostPath(
			thread.channelId,
			thread.postId,
			u"/comments/%1/reactions"_q.arg(item->id.bare)),
		QJsonObject{ { "reactions", ids } },
		[=](QJsonDocument doc, QString error, int) {
			const auto strong = weak.get();
			const auto bridge = strong ? BridgeFor(strong) : nullptr;
			if (!bridge || !error.isEmpty()) {
				return;
			}
			ApplyComment(bridge, history, thread, doc.object(), NewMessageType::Existing);
		});
}

GifSource GifSourceOf(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (IsDiscussionPeer(peer)) {
		return {
			.chatId = ChannelOfDiscussion(qint64(peerToChannel(peer->id).bare)),
			.threadRootId = RootOf(item).bare,
		};
	} else if (IsTopicChannelPeer(peer)) {
		return {
			.chatId = qint64(peerToChannel(peer->id).bare),
			.messageId = item->id.bare,
		};
	}
	return {};
}

QJsonObject KeepOwnReactions(HistoryItem *item, QJsonObject message) {
	if (!item) {
		return message;
	}
	const auto chosen = item->chosenReactions();
	auto reactions = message.value("reactions").toArray();
	for (auto i = 0; i != reactions.size(); ++i) {
		auto reaction = reactions.at(i).toObject();
		const auto emojiId = DocumentId(Number(reaction, "emoji_id"));
		const auto own = ranges::any_of(chosen, [&](const Data::ReactionId &id) {
			return id.custom() == emojiId;
		});
		reaction.insert("chosen", own);
		reactions.replace(i, reaction);
	}
	message.insert("reactions", reactions);
	return message;
}

void ApplyDiscussionRead(
		not_null<NativeBridge*> bridge,
		const QJsonObject &data) {
	const auto channelId = Number(data, "chat_id");
	const auto postId = Number(data, "post_id");
	const auto rootId = Number(data, "root_id");
	const auto readTill = MsgId(Number(data, "read_max_id"));
	if (channelId <= 0 || rootId <= 0 || readTill <= 0) {
		return;
	}
	RememberRoot(channelId, rootId, postId);
	auto &owner = bridge->session().data();
	const auto root = FullMsgId(
		peerFromChannel(ChannelId(DiscussionId(channelId))),
		MsgId(rootId));
	owner.updateRepliesReadTill({ root, readTill, false });
	if (const auto item = owner.message(root)) {
		item->setCommentsInboxReadTill(readTill);
	}
	if (const auto post = owner.message(
			peerFromChannel(ChannelId(channelId)),
			MsgId(postId))) {
		post->setCommentsInboxReadTill(readTill);
	}
}

void ApplyCommentEvent(
		not_null<NativeBridge*> bridge,
		const QJsonObject &data) {
	const auto channelId = Number(data, "chat_id");
	const auto postId = Number(data, "post_id");
	const auto rootId = Number(data, "root_id");
	RememberRoot(channelId, rootId, postId);
	const auto chat = bridge->communityChat(channelId);
	if (chat.isEmpty()) {
		return;
	}
	const auto session = &bridge->session();
	bridge->applyCommunityChat(DiscussionChat(chat), false);
	if (const auto post = session->data().message(
			peerFromChannel(ChannelId(channelId)),
			MsgId(postId))) {
		const auto replies = Replies(
			QJsonObject{
				{ "count", data.value("count") },
				{ "max_id", data.value("max_id") },
			},
			channelId);
		post->setReplies(HistoryMessageRepliesData(&replies));
	}
	const auto history = session->data().history(
		peerFromChannel(ChannelId(DiscussionId(channelId))));
	auto comment = data;
	auto message = comment.value("message").toObject();
	message = KeepOwnReactions(
		session->data().message(
			history->peer,
			MsgId(Number(message, "id"))),
		message);
	if (data.value("addressed").toBool()) {
		message.insert("mentioned", true);
	}
	comment.insert("message", message);
	ApplyComment(
		bridge,
		history,
		Thread{ .channelId = channelId, .rootId = rootId, .postId = postId },
		comment,
		data.value("created").toBool()
			? NewMessageType::Unread
			: NewMessageType::Existing);
}

}
