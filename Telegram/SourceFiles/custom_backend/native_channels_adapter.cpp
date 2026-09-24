/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_channels_adapter.h"

#include "base/debug_log.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_community.h"
#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_runtime.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>

#include <crl/crl_on_main.h>

namespace CustomBackend::Channels {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;
using Mtp::Answer;
using Mtp::Reader;

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

[[nodiscard]] qint64 ChatId(const MTPInputChannel &channel) {
	return channel.match([](const MTPDinputChannel &data) {
		return qint64(data.vchannel_id().v);
	}, [](const MTPDinputChannelFromMessage &data) {
		return qint64(data.vchannel_id().v);
	}, [](const MTPDinputChannelEmpty &) {
		return qint64(0);
	});
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

[[nodiscard]] qint64 UserIdOf(const MTPInputPeer &peer, qint64 me) {
	return peer.match([](const MTPDinputPeerUser &data) {
		return qint64(data.vuser_id().v);
	}, [](const MTPDinputPeerUserFromMessage &data) {
		return qint64(data.vuser_id().v);
	}, [&](const MTPDinputPeerSelf &) {
		return me;
	}, [](const auto &) {
		return qint64(0);
	});
}

[[nodiscard]] qint64 UserIdOf(const MTPInputUser &user, qint64 me) {
	return user.match([](const MTPDinputUser &data) {
		return qint64(data.vuser_id().v);
	}, [](const MTPDinputUserFromMessage &data) {
		return qint64(data.vuser_id().v);
	}, [&](const MTPDinputUserSelf &) {
		return me;
	}, [](const auto &) {
		return qint64(0);
	});
}

[[nodiscard]] QString OperationId() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] QString ChatPath(qint64 chatId, const QString &tail = QString()) {
	return u"/chats/%1%2"_q.arg(chatId).arg(tail);
}

[[nodiscard]] MTPUpdates Updates(
		QVector<MTPUpdate> list = {},
		QVector<MTPChat> chats = {}) {
	return MTP_updates(
		MTP_vector<MTPUpdate>(std::move(list)),
		MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>(std::move(chats)),
		MTP_int(base::unixtime::now()),
		MTP_int(0));
}

// Runs a REST call and turns a refusal into the MTProto error upstream reacts
// to; fxl-api names it in "code" (USERNAME_OCCUPIED, CHAT_ADMIN_REQUIRED...).
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
				u"CHANNEL_INVALID"_q,
				u"CHAT_ADMIN_REQUIRED"_q);
			LOG(("FoxMes Channels: %1 %2 failed, status %3, error %4").arg(
				QString::fromLatin1(method),
				path,
				QString::number(status),
				type));
			answer.fail(type, (status >= 400 && status < 600) ? status : 400);
			return;
		}
		done(doc.object());
	});
}

// After an answer upstream applied through its own appliers (channelFull,
// chats in updates) the bridge puts back what they reset. Queued: the answer
// is being processed when this is called.
void Reapply(const Context &context, qint64 chatId) {
	crl::on_main([session = context.session, chatId] {
		if (const auto strong = session.get()) {
			if (const auto bridge = BridgeFor(strong)) {
				bridge->reapplyCommunityChat(chatId);
			}
		}
	});
}

// The profile a mutation answered with, applied at once: the event carrying
// the same change reaches every member, this one included, but the box that
// made the change should not wait for it.
void MergeProfile(const Context &context, const QJsonObject &profile) {
	if (const auto bridge = BridgeOf(context)) {
		bridge->applyCommunityProfile(profile);
	}
}

[[nodiscard]] TimeId TtlFor(const Context &context, qint64 chatId) {
	const auto session = context.session.get();
	if (!session) {
		return 0;
	}
	return AutoDeletePeriod(session->data().channel(ChannelId(chatId)));
}

[[nodiscard]] MTPmessages_ChatFull FullFor(
		const Context &context,
		not_null<NativeBridge*> bridge,
		const QJsonObject &chat) {
	const auto chatId = chat.value("id").toVariant().toLongLong();
	return MTP_messages_chatFull(
		Community::ChannelFull(
			chat,
			context.me,
			bridge->communityNotifySettings(chatId, chat),
			TtlFor(context, chatId)),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>());
}

// --- the requests ------------------------------------------------------

void GetFullChannel(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	context.client->communityRequest("GET", ChatPath(chatId), {}, [=](
			QJsonDocument doc,
			QString error,
			int) {
		const auto bridge = BridgeOf(context);
		if (!bridge) {
			answer.fail(u"CHANNEL_INVALID"_q);
			return;
		}
		auto chat = doc.object();
		if (error.isEmpty()) {
			bridge->applyCommunityChat(chat);
		} else {
			// A public chat opened from its link before joining: the server
			// shows it only to members, the preview it resolved to is here.
			chat = bridge->communityChat(chatId);
			if (chat.isEmpty()) {
				answer.fail(u"CHANNEL_PRIVATE"_q);
				return;
			}
		}
		answer.done(FullFor(context, bridge, chat));
		Reapply(context, chatId);
	});
}

void GetChannels(const Context &context, Reader &reader) {
	const auto list = reader.read<MTPVector<MTPInputChannel>>();
	const auto bridge = BridgeOf(context);
	if (!reader.ok() || !bridge) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	auto chats = QVector<MTPChat>();
	for (const auto &input : list.v) {
		const auto chatId = ChatId(input);
		const auto chat = bridge->communityChat(chatId);
		if (!chat.isEmpty()) {
			chats.push_back(Community::Channel(chat, context.me));
			Reapply(context, chatId);
		}
	}
	context.answer.done(MTP_messages_chats(MTP_vector<MTPChat>(chats)));
}

void GetParticipants(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto filter = reader.read<MTPChannelParticipantsFilter>();
	const auto offset = reader.read<MTPint>().v;
	const auto limit = reader.read<MTPint>().v;
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	auto query = QUrlQuery();
	filter.match([&](const MTPDchannelParticipantsRecent &) {
		query.addQueryItem(u"filter"_q, u"recent"_q);
	}, [&](const MTPDchannelParticipantsAdmins &) {
		query.addQueryItem(u"filter"_q, u"admins"_q);
	}, [&](const MTPDchannelParticipantsKicked &) {
		query.addQueryItem(u"filter"_q, u"kicked"_q);
	}, [&](const MTPDchannelParticipantsBots &) {
		query.addQueryItem(u"filter"_q, u"bots"_q);
	}, [&](const MTPDchannelParticipantsBanned &) {
		query.addQueryItem(u"filter"_q, u"banned"_q);
	}, [&](const MTPDchannelParticipantsSearch &data) {
		query.addQueryItem(u"filter"_q, u"search"_q);
		query.addQueryItem(u"q"_q, qs(data.vq()));
	}, [&](const MTPDchannelParticipantsContacts &data) {
		query.addQueryItem(u"filter"_q, u"search"_q);
		query.addQueryItem(u"q"_q, qs(data.vq()));
	}, [&](const MTPDchannelParticipantsMentions &data) {
		query.addQueryItem(u"filter"_q, u"search"_q);
		query.addQueryItem(u"q"_q, qs(data.vq().value_or_empty()));
	});
	query.addQueryItem(u"offset"_q, QString::number(offset));
	query.addQueryItem(u"limit"_q, QString::number(limit));
	const auto path = ChatPath(chatId, u"/members?"_q
		+ query.toString(QUrl::FullyEncoded));
	const auto answer = context.answer;
	const auto me = context.me;
	Request(context, "GET", path, {}, [=](QJsonObject data) {
		const auto bridge = BridgeOf(context);
		auto list = QVector<MTPChannelParticipant>();
		for (const auto &entry : data.value("items").toArray()) {
			const auto member = entry.toObject();
			if (bridge) {
				bridge->ensureUser(member.value("user").toObject(), false);
			}
			list.push_back(Community::Participant(member, me));
		}
		answer.done(MTP_channels_channelParticipants(
			MTP_int(data.value("count").toInt()),
			MTP_vector<MTPChannelParticipant>(list),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	});
}

void GetParticipant(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto userId = UserIdOf(reader.read<MTPInputPeer>(), context.me);
	if (!reader.ok() || chatId <= 0 || userId <= 0) {
		context.answer.fail(u"PARTICIPANT_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	const auto me = context.me;
	Request(context, "GET", ChatPath(chatId, u"/members/%1"_q.arg(userId)), {}, [=](
			QJsonObject member) {
		if (const auto bridge = BridgeOf(context)) {
			bridge->ensureUser(member.value("user").toObject(), false);
		}
		answer.done(MTP_channels_channelParticipant(
			Community::Participant(member, me),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	});
}

void CreateChannel(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto title = qs(reader.read<MTPstring>());
	const auto about = qs(reader.read<MTPstring>());
	if (!reader.ok()) {
		context.answer.fail(u"CHAT_TITLE_EMPTY"_q);
		return;
	}
	const auto broadcast = (flags & (1 << 0)) != 0;
	const auto answer = context.answer;
	const auto me = context.me;
	Request(context, "POST", u"/chats"_q, {
		{ "type", broadcast ? u"channel"_q : u"group"_q },
		{ "title", title },
		{ "about", about },
		{ "operation_id", OperationId() },
	}, [=](QJsonObject chat) {
		const auto chatId = chat.value("id").toVariant().toLongLong();
		if (const auto bridge = BridgeOf(context)) {
			bridge->applyCommunityChat(chat);
		}
		// GroupInfoBox takes the new channel from the chats of the answer.
		answer.done(Updates(
			{ MTP_updateChannel(MTP_long(chatId)) },
			{ Community::Channel(chat, me) }));
		Reapply(context, chatId);
	});
}

void Patch(
		const Context &context,
		qint64 chatId,
		QJsonObject body,
		Fn<void()> done) {
	body.insert("operation_id", OperationId());
	Request(context, "PATCH", ChatPath(chatId), body, [=](QJsonObject profile) {
		MergeProfile(context, profile);
		done();
	});
}

void EditTitle(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto title = qs(reader.read<MTPstring>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Patch(context, chatId, { { "title", title } }, [=] {
		answer.done(Updates());
	});
}

void EditAbout(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	const auto about = qs(reader.read<MTPstring>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Patch(context, chatId, { { "about", about } }, [=] {
		answer.done(MTP_boolTrue());
	});
}

// Only the removal comes here: a new photo is uploaded by the bridge
// (UploadPeerPhoto), since upstream's uploader speaks MTProto file parts.
void EditPhoto(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto photo = reader.read<MTPInputChatPhoto>();
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	} else if (photo.type() != mtpc_inputChatPhotoEmpty) {
		context.answer.fail(u"PHOTO_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "DELETE", ChatPath(chatId, u"/photo"_q), {}, [=](
			QJsonObject profile) {
		MergeProfile(context, profile);
		answer.done(Updates());
	});
}

void TogglePreHistoryHidden(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto enabled = reader.read<MTPBool>();
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Patch(context, chatId, { { "history_hidden", mtpIsTrue(enabled) } }, [=] {
		answer.done(Updates());
	});
}

void ToggleSignatures(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Patch(context, chatId, { { "signatures", (flags & (1 << 0)) != 0 } }, [=] {
		answer.done(Updates());
	});
}

void CheckUsername(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto name = qs(reader.read<MTPstring>());
	if (!reader.ok()) {
		context.answer.fail(u"USERNAME_INVALID"_q);
		return;
	}
	auto query = QUrlQuery();
	query.addQueryItem(u"name"_q, name);
	if (chatId > 0) {
		query.addQueryItem(u"chat_id"_q, QString::number(chatId));
	}
	const auto answer = context.answer;
	Request(
		context,
		"GET",
		u"/public-names/check?"_q + query.toString(QUrl::FullyEncoded),
		{},
		[=](QJsonObject data) {
			if (data.value("available").toBool()) {
				answer.done(MTP_boolTrue());
			} else if (data.value("reason").toString() == u"USERNAME_INVALID"_q) {
				answer.fail(u"USERNAME_INVALID"_q);
			} else {
				answer.done(MTP_boolFalse());
			}
		});
}

void SetUsername(const Context &context, qint64 chatId, const QString &name) {
	const auto answer = context.answer;
	Request(context, "PUT", ChatPath(chatId, u"/username"_q), {
		{ "username", name },
		{ "operation_id", OperationId() },
	}, [=](QJsonObject profile) {
		MergeProfile(context, profile);
		answer.done(MTP_boolTrue());
	});
}

void UpdateUsername(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto name = qs(reader.read<MTPstring>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	SetUsername(context, chatId, name);
}

void DeactivateAllUsernames(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	SetUsername(context, chatId, QString());
}

// A FoxMes chat has one public name, so there is no order to change and no
// second name to switch; the collectible-names list is hidden.
void AcceptUsernames(const Context &context, Reader &) {
	context.answer.done(MTP_boolTrue());
}

void InviteToChannel(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto users = reader.read<MTPVector<MTPInputUser>>();
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	auto ids = QJsonArray();
	for (const auto &user : users.v) {
		if (const auto id = UserIdOf(user, context.me)) {
			ids.push_back(id);
		}
	}
	const auto answer = context.answer;
	Request(context, "POST", ChatPath(chatId, u"/members"_q), {
		{ "user_ids", ids },
		{ "operation_id", OperationId() },
	}, [=](QJsonObject) {
		// The members arrive with chat.member_added and the service message.
		answer.done(MTP_messages_invitedUsers(
			Updates(),
			MTP_vector<MTPMissingInvitee>()));
	});
}

// Upstream removes a member by banning view_messages. FoxMes has no banned
// list, so any other rights change is accepted and changes nothing.
void EditBanned(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	const auto userId = UserIdOf(reader.read<MTPInputPeer>(), context.me);
	const auto rights = reader.read<MTPChatBannedRights>();
	if (!reader.ok() || chatId <= 0 || userId <= 0) {
		context.answer.fail(u"PARTICIPANT_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	if (!rights.data().is_view_messages()) {
		answer.done(Updates());
		return;
	}
	auto query = QUrlQuery();
	query.addQueryItem(u"operation_id"_q, OperationId());
	Request(
		context,
		"DELETE",
		ChatPath(chatId, u"/members/%1?"_q.arg(userId)
			+ query.toString(QUrl::FullyEncoded)),
		{},
		[=](QJsonObject) { answer.done(Updates()); });
}

void JoinChannel(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	const auto me = context.me;
	Request(context, "POST", ChatPath(chatId, u"/join"_q), {
		{ "operation_id", OperationId() },
	}, [=](QJsonObject chat) {
		if (const auto bridge = BridgeOf(context)) {
			bridge->applyCommunityChat(chat);
		}
		answer.done(MTP_messages_chatInviteJoinResultOk(Updates(
			{ MTP_updateChannel(MTP_long(chatId)) },
			{ Community::Channel(chat, me) })));
		Reapply(context, chatId);
	});
}

void LeaveChannel(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "POST", ChatPath(chatId, u"/leave"_q), {
		{ "operation_id", OperationId() },
	}, [=](QJsonObject) {
		// chat.deleted takes the chat out of the list; until it arrives the
		// channel reads as left.
		if (const auto bridge = BridgeOf(context)) {
			const auto chat = bridge->communityChat(chatId);
			if (!chat.isEmpty()) {
				bridge->applyCommunityChat(chat, false);
			}
		}
		answer.done(Updates());
	});
}

void DeleteChannel(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputChannel>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"CHANNEL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "DELETE", ChatPath(chatId), {}, [=](QJsonObject) {
		answer.done(Updates());
	});
}

// The creator is the only admin and has no successor: upstream hears that and
// offers to delete the chat instead of leaving it (DeleteAndLeaveHandler).
void FutureCreator(const Context &context, Reader &) {
	context.answer.fail(u"CHAT_OWNER_LEAVE"_q);
}

void ResolveUsername(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto name = qs(reader.read<MTPstring>());
	if (flags & (1 << 0)) {
		[[maybe_unused]] const auto referer = reader.read<MTPstring>();
	}
	if (!reader.ok() || name.isEmpty()) {
		context.answer.fail(u"USERNAME_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	const auto path = u"/resolve/"_q
		+ QString::fromLatin1(QUrl::toPercentEncoding(name));
	Request(context, "GET", path, {}, [=](QJsonObject data) {
		const auto bridge = BridgeOf(context);
		if (!bridge) {
			answer.fail(u"USERNAME_NOT_OCCUPIED"_q);
			return;
		}
		const auto user = data.value("user").toObject();
		if (!user.isEmpty()) {
			bridge->ensureUser(user, false);
			answer.done(MTP_contacts_resolvedPeer(
				MTP_peerUser(MTP_long(user.value("id").toVariant().toLongLong())),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>()));
			return;
		}
		const auto chat = data.value("chat").toObject();
		const auto chatId = chat.value("id").toVariant().toLongLong();
		if (chatId <= 0) {
			answer.fail(u"USERNAME_NOT_OCCUPIED"_q);
			return;
		}
		bridge->applyCommunityChat(chat, data.value("member").toBool());
		answer.done(MTP_contacts_resolvedPeer(
			MTP_peerChannel(MTP_long(chatId)),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	});
}

// The permanent link upstream creates right after a new channel
// (GroupInfoBox::checkInviteLink) and waits for before it goes on. Invite
// links are hidden in FoxMes; the answer only lets that flow finish.
void ExportChatInvite(const Context &context, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	using Flag = MTPDchatInviteExported::Flag;
	context.answer.done(MTP_chatInviteExported(
		MTP_flags(Flag::f_permanent),
		MTP_string(u"https://fxl.ru/+%1"_q.arg(chatId)),
		MTP_long(context.me),
		MTP_int(base::unixtime::now()),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPstring(),
		MTPStarsSubscriptionPricing()));
}

void NoInvites(const Context &context, Reader &) {
	context.answer.done(MTP_messages_exportedChatInvites(
		MTP_int(0),
		MTP_vector<MTPExportedChatInvite>(),
		MTP_vector<MTPUser>()));
}

void NoAdminsWithInvites(const Context &context, Reader &) {
	context.answer.done(MTP_messages_chatAdminsWithInvites(
		MTP_vector<MTPChatAdminWithInvites>(),
		MTP_vector<MTPUser>()));
}

// Upstream asks once for every channel post it shows, with increment, and
// draws the counts it gets back (Api::ViewsManager). The answer has to name
// every requested post, in order: ViewsManager drops a shorter one whole.
void GetMessagesViews(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	const auto ids = reader.read<MTPVector<MTPint>>();
	const auto increment = mtpIsTrue(reader.read<MTPBool>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	auto list = QJsonArray();
	for (const auto &id : ids.v) {
		list.push_back(id.v);
	}
	const auto answer = context.answer;
	Request(context, "POST", ChatPath(chatId, u"/views"_q), {
		{ "ids", list },
		{ "increment", increment },
	}, [=](QJsonObject data) {
		auto counts = base::flat_map<int, int>();
		for (const auto &entry : data.value("views").toArray()) {
			const auto object = entry.toObject();
			counts[object.value("id").toInt()] = object.value("views").toInt();
		}
		auto views = QVector<MTPMessageViews>();
		views.reserve(ids.v.size());
		for (const auto &id : ids.v) {
			const auto i = counts.find(id.v);
			views.push_back(MTP_messageViews(
				MTP_flags(MTPDmessageViews::Flag::f_views),
				MTP_int((i != counts.end()) ? i->second : 0),
				MTPint(),
				MTPMessageReplies()));
		}
		answer.done(MTP_messages_messageViews(
			MTP_vector<MTPMessageViews>(views),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	});
}

void NoSendAs(const Context &context, Reader &) {
	context.answer.done(MTP_channels_sendAsPeers(
		MTP_vector<MTPSendAsPeer>(),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>()));
}

void NoSponsored(const Context &context, Reader &) {
	context.answer.done(MTP_messages_sponsoredMessagesEmpty());
}

void NoRecommendations(const Context &context, Reader &) {
	context.answer.done(MTP_messages_chats(MTP_vector<MTPChat>()));
}

// --- welcome messages --------------------------------------------------

[[nodiscard]] QString WelcomePath(qint64 chatId, qint64 id = 0) {
	return ChatPath(chatId, id
		? u"/welcome-messages/%1"_q.arg(id)
		: u"/welcome-messages"_q);
}

void GetWelcomeMessages(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "GET", WelcomePath(chatId), {}, [=](QJsonObject data) {
		const auto bridge = BridgeOf(context);
		auto list = QVector<MTPEphemeralMessage>();
		for (const auto &entry : data.value("items").toArray()) {
			if (bridge) {
				list.push_back(bridge->welcomeMessage(entry.toObject(), true));
			}
		}
		answer.done(MTP_ephemeral_welcomeMessages(
			MTP_long(0),
			MTP_vector<MTPEphemeralMessage>(list)));
	});
}

[[nodiscard]] QJsonObject WelcomeBody(
		const Context &context,
		const QString &text,
		const MTPVector<MTPMessageEntity> &entities) {
	auto result = QJsonObject{ { "text", text } };
	if (const auto bridge = BridgeOf(context)) {
		result.insert("entities", bridge->entitiesJson(entities, text));
	}
	return result;
}

// Text templates only: fxl-api keeps no media for welcome messages.
void SendWelcome(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	if (!(flags & (1 << 7)) || !(flags & (1 << 8)) || (flags & (1 << 2))) {
		context.answer.fail(u"MEDIA_INVALID"_q);
		return;
	}
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	[[maybe_unused]] const auto receiver = reader.read<MTPInputUser>();
	if (flags & (1 << 0)) {
		[[maybe_unused]] const auto queryId = reader.read<MTPlong>();
	}
	const auto text = qs(reader.read<MTPstring>());
	const auto entities = (flags & (1 << 1))
		? reader.read<MTPVector<MTPMessageEntity>>()
		: MTPVector<MTPMessageEntity>();
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	auto body = WelcomeBody(context, text, entities);
	body.insert("operation_id", OperationId());
	const auto answer = context.answer;
	Request(context, "POST", WelcomePath(chatId), body, [=](QJsonObject data) {
		const auto bridge = BridgeOf(context);
		if (!bridge) {
			answer.fail(u"PEER_ID_INVALID"_q);
			return;
		}
		answer.done(Updates({ MTP_updateNewEphemeralMessage(
			bridge->welcomeMessage(data, true)) }));
	});
}

void EditWelcome(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	if (!(flags & (1 << 6)) || !(flags & (1 << 7)) || (flags & (1 << 3))) {
		context.answer.fail(u"MEDIA_INVALID"_q);
		return;
	}
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	[[maybe_unused]] const auto receiver = reader.read<MTPInputUser>();
	const auto id = reader.read<MTPint>().v;
	const auto text = (flags & (1 << 0))
		? qs(reader.read<MTPstring>())
		: QString();
	const auto entities = (flags & (1 << 1))
		? reader.read<MTPVector<MTPMessageEntity>>()
		: MTPVector<MTPMessageEntity>();
	if (!reader.ok() || chatId <= 0 || id <= 0) {
		context.answer.fail(u"MESSAGE_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(
		context,
		"PATCH",
		WelcomePath(chatId, id),
		WelcomeBody(context, text, entities),
		[=](QJsonObject data) {
			const auto bridge = BridgeOf(context);
			if (!bridge) {
				answer.fail(u"PEER_ID_INVALID"_q);
				return;
			}
			answer.done(Updates({ MTP_updateEditEphemeralMessage(
				bridge->welcomeMessage(data, true)) }));
		});
}

void DeleteWelcome(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	const auto id = reader.read<MTPint>().v;
	if (!reader.ok() || chatId <= 0 || id <= 0) {
		context.answer.fail(u"MESSAGE_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "DELETE", WelcomePath(chatId, id), {}, [=](QJsonObject) {
		answer.done(MTP_boolTrue());
	});
}

void DeleteAllWelcome(const Context &context, Reader &reader) {
	const auto chatId = ChatId(reader.read<MTPInputPeer>());
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "DELETE", WelcomePath(chatId), {}, [=](QJsonObject) {
		answer.done(MTP_boolTrue());
	});
}

using Handler = void(*)(const Context &, Reader &);

[[nodiscard]] Handler HandlerFor(mtpTypeId type) {
	switch (type) {
	case mtpc_channels_getFullChannel: return GetFullChannel;
	case mtpc_channels_getChannels: return GetChannels;
	case mtpc_channels_getParticipants: return GetParticipants;
	case mtpc_channels_getParticipant: return GetParticipant;
	case mtpc_channels_createChannel: return CreateChannel;
	case mtpc_channels_editTitle: return EditTitle;
	case mtpc_messages_editChatAbout: return EditAbout;
	case mtpc_channels_editPhoto: return EditPhoto;
	case mtpc_channels_togglePreHistoryHidden: return TogglePreHistoryHidden;
	case mtpc_channels_toggleSignatures: return ToggleSignatures;
	case mtpc_channels_checkUsername: return CheckUsername;
	case mtpc_channels_updateUsername: return UpdateUsername;
	case mtpc_channels_deactivateAllUsernames: return DeactivateAllUsernames;
	case mtpc_channels_reorderUsernames: return AcceptUsernames;
	case mtpc_channels_toggleUsername: return AcceptUsernames;
	case mtpc_channels_inviteToChannel: return InviteToChannel;
	case mtpc_channels_editBanned: return EditBanned;
	case mtpc_channels_joinChannel: return JoinChannel;
	case mtpc_channels_leaveChannel: return LeaveChannel;
	case mtpc_channels_deleteChannel: return DeleteChannel;
	case mtpc_messages_getFutureChatCreatorAfterLeave: return FutureCreator;
	case mtpc_contacts_resolveUsername: return ResolveUsername;
	case mtpc_messages_exportChatInvite: return ExportChatInvite;
	case mtpc_messages_getExportedChatInvites: return NoInvites;
	case mtpc_messages_getAdminsWithInvites: return NoAdminsWithInvites;
	case mtpc_channels_getSendAs: return NoSendAs;
	case mtpc_messages_getMessagesViews: return GetMessagesViews;
	case mtpc_messages_getSponsoredMessages: return NoSponsored;
	case mtpc_channels_getChannelRecommendations: return NoRecommendations;
	case mtpc_ephemeral_getWelcomeMessages: return GetWelcomeMessages;
	case mtpc_ephemeral_sendMessage: return SendWelcome;
	case mtpc_ephemeral_editMessage: return EditWelcome;
	case mtpc_ephemeral_deleteWelcomeMessage: return DeleteWelcome;
	case mtpc_ephemeral_deleteAllWelcomeMessages: return DeleteAllWelcome;
	}
	return nullptr;
}

} // namespace

bool Intercepts(const SerializedRequest &request) {
	return HandlerFor(Mtp::RequestType(request)) != nullptr;
}

void Intercept(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const SerializedRequest &request) {
	const auto handler = HandlerFor(Mtp::RequestType(request));
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

} // namespace CustomBackend::Channels
