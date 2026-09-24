/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_community.h"

#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>

namespace CustomBackend::Community {
namespace {

[[nodiscard]] qint64 Number(const QJsonObject &data, const char *key) {
	return data.value(QLatin1String(key)).toVariant().toLongLong();
}

[[nodiscard]] TimeId Time(const QJsonObject &data, const char *key) {
	const auto value = QDateTime::fromString(
		data.value(QLatin1String(key)).toString(),
		Qt::ISODate);
	return value.isValid() ? TimeId(value.toSecsSinceEpoch()) : TimeId(0);
}

[[nodiscard]] bool IsOwner(const QJsonObject &chat, qint64 me) {
	return me > 0 && OwnerId(chat) == me;
}

// Members may not change the group info, create polls or topics: the creator
// is the only admin, the permissions screen is hidden, and polls are not part
// of FoxMes groups. Everything else is allowed, like upstream's defaults.
[[nodiscard]] MTPChatBannedRights MemberRestrictions() {
	using Flag = MTPDchatBannedRights::Flag;
	return MTP_chatBannedRights(
		MTP_flags(Flag::f_change_info
			| Flag::f_send_polls
			| Flag::f_manage_topics),
		MTP_int(0));
}

[[nodiscard]] MTPInputGroupCall InputCall(const QJsonObject &call) {
	return MTP_inputGroupCall(
		MTP_long(Number(call, "id")),
		MTP_long(Number(call, "access_hash")));
}

[[nodiscard]] QString UserName(
		not_null<Main::Session*> session,
		qint64 userId) {
	if (const auto user = session->data().userLoaded(UserId(userId))) {
		return user->name();
	}
	return QString();
}

} // namespace

bool IsCommunity(const QJsonObject &chat) {
	const auto type = chat.value("type").toString();
	return type == u"group"_q || type == u"channel"_q;
}

bool IsChannel(const QJsonObject &chat) {
	return chat.value("type").toString() == u"channel"_q;
}

qint64 OwnerId(const QJsonObject &chat) {
	return Number(chat, "owner_id");
}

MTPChatAdminRights OwnerRights() {
	using Flag = MTPDchatAdminRights::Flag;
	return MTP_chatAdminRights(MTP_flags(Flag::f_change_info
		| Flag::f_post_messages
		| Flag::f_edit_messages
		| Flag::f_delete_messages
		| Flag::f_ban_users
		| Flag::f_invite_users
		| Flag::f_pin_messages
		| Flag::f_manage_call
		| Flag::f_other
		| Flag::f_manage_welcome_messages));
}

MTPChat Channel(const QJsonObject &chat, qint64 me, bool member) {
	using Flag = MTPDchannel::Flag;
	const auto channel = IsChannel(chat);
	const auto owner = IsOwner(chat, me);
	const auto username = chat.value("username").toString();
	const auto call = chat.value("call").toObject();
	auto flags = Flag::f_access_hash
		| Flag::f_participants_count
		| (channel ? Flag::f_broadcast : Flag::f_megagroup);
	if (!channel) {
		flags |= Flag::f_default_banned_rights;
	}
	if (owner) {
		flags |= Flag::f_creator | Flag::f_admin_rights;
	}
	if (!member) {
		flags |= Flag::f_left;
	}
	if (!username.isEmpty()) {
		flags |= Flag::f_username;
	}
	if (channel && chat.value("signatures").toBool()) {
		flags |= Flag::f_signatures;
	}
	if (!call.isEmpty()) {
		flags |= Flag::f_call_active;
		if (call.value("participants_count").toInt() > 0) {
			flags |= Flag::f_call_not_empty;
		}
	}
	const auto id = Number(chat, "id");
	return MTP_channel(
		MTP_flags(flags),
		MTP_long(id),
		// Never sent anywhere: the bridge addresses a chat by its id. Upstream
		// still expects a known hash on a channel it may write to.
		MTP_long(id),
		MTP_string(chat.value("title").toString()),
		MTP_string(username),
		MTP_chatPhotoEmpty(),
		MTP_int(Time(chat, "created_at")),
		MTPVector<MTPRestrictionReason>(),
		owner ? OwnerRights() : MTPChatAdminRights(),
		MTPChatBannedRights(),
		channel ? MTPChatBannedRights() : MemberRestrictions(),
		MTP_int(chat.value("participants_count").toInt()),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPEmojiStatus(),
		MTPint(), // level
		MTPint(), // subscription_until_date
		MTPlong(), // bot_verification_icon
		MTPlong(), // send_paid_messages_stars
		MTPlong(), // linked_monoforum_id
		MTPlong()); // linked_community_id
}

MTPChatFull ChannelFull(
		const QJsonObject &chat,
		qint64 me,
		const MTPPeerNotifySettings &notify,
		TimeId ttlPeriod) {
	using Flag = MTPDchannelFull::Flag;
	const auto channel = IsChannel(chat);
	const auto owner = IsOwner(chat, me);
	const auto call = chat.value("call").toObject();
	const auto floor = chat.value("available_min_id").toInt();
	auto flags = Flag::f_participants_count
		| Flag::f_admins_count
		| Flag::f_available_reactions;
	// A group shows its members to everybody, a channel its subscribers to
	// the owner only - the same rule fxl-api applies to /members.
	if (!channel || owner) {
		flags |= Flag::f_can_view_participants;
	}
	if (owner) {
		flags |= Flag::f_can_set_username | Flag::f_can_delete_channel;
	}
	if (!channel && chat.value("history_hidden").toBool()) {
		flags |= Flag::f_hidden_prehistory;
	}
	if (floor > 0) {
		flags |= Flag::f_available_min_id;
	}
	if (!call.isEmpty()) {
		flags |= Flag::f_call;
	}
	if (ttlPeriod > 0) {
		flags |= Flag::f_ttl_period;
	}
	const auto theme = chat.value("theme_emoticon").toString();
	if (!theme.isEmpty()) {
		flags |= Flag::f_theme_emoticon;
	}
	if (chat.value("has_welcome_messages").toBool()) {
		flags |= Flag::f_has_welcome_messages;
	}
	return MTP_channelFull(
		MTP_flags(flags),
		MTP_long(Number(chat, "id")),
		MTP_string(chat.value("about").toString()),
		MTP_int(chat.value("participants_count").toInt()),
		MTP_int(1), // admins_count: the creator
		MTPint(), // kicked_count
		MTPint(), // banned_count
		MTPint(), // online_count
		MTP_int(0), // read_inbox_max_id, not applied (see pts)
		MTP_int(0), // read_outbox_max_id
		MTP_int(0), // unread_count
		MTP_photoEmpty(MTP_long(0)),
		notify,
		MTPExportedChatInvite(),
		MTP_vector<MTPBotInfo>(),
		MTPlong(), // migrated_from_chat_id
		MTPint(), // migrated_from_max_id
		MTPint(), // pinned_msg_id: pins are synced by the bridge
		MTPStickerSet(),
		MTP_int(floor),
		MTPint(), // folder_id
		MTPlong(), // linked_chat_id
		MTPChannelLocation(),
		MTPint(), // slowmode_seconds
		MTPint(), // slowmode_next_send_date
		MTPint(), // stats_dc
		MTP_int(1), // pts
		call.isEmpty() ? MTPInputGroupCall() : InputCall(call),
		MTP_int(ttlPeriod),
		MTPVector<MTPstring>(),
		MTPPeer(), // groupcall_default_join_as
		MTP_string(theme),
		MTPint(), // requests_pending
		MTPVector<MTPlong>(),
		MTPPeer(), // default_send_as
		MTP_chatReactionsAll(MTP_flags(0)),
		MTPint(), // reactions_limit
		MTPPeerStories(),
		MTPWallPaper(),
		MTPint(), // boosts_applied
		MTPint(), // boosts_unrestrict
		MTPStickerSet(),
		MTPBotVerification(),
		MTPint(), // stargifts_count
		MTPlong(), // send_paid_messages_stars
		MTPProfileTab(),
		MTPlong()); // guard_bot_id
}

MTPChannelParticipant Participant(const QJsonObject &member, qint64 me) {
	const auto user = member.value("user").toObject();
	const auto userId = Number(user, "id");
	if (member.value("role").toString() == u"creator"_q) {
		return MTP_channelParticipantCreator(
			MTP_flags(0),
			MTP_long(userId),
			OwnerRights(),
			MTPstring());
	}
	const auto date = MTP_int(Time(member, "joined_at"));
	if (userId == me) {
		// Only this form tells upstream that "me" is in the chat.
		return MTP_channelParticipantSelf(
			MTP_flags(0),
			MTP_long(userId),
			MTP_long(Number(member, "invited_by")),
			date,
			MTPint(),
			MTPstring());
	}
	return MTP_channelParticipant(
		MTP_flags(0),
		MTP_long(userId),
		date,
		MTPint(),
		MTPstring());
}

MTPMessageAction Action(
		not_null<Main::Session*> session,
		const QJsonObject &action,
		qint64 actorId,
		bool channel) {
	const auto type = action.value("type").toString();
	auto users = QVector<MTPlong>();
	for (const auto &id : action.value("user_ids").toArray()) {
		users.push_back(MTP_long(id.toVariant().toLongLong()));
	}
	const auto call = MTP_inputGroupCall(
		MTP_long(Number(action, "call_id")),
		MTP_long(0));
	if (type == u"created"_q) {
		return MTP_messageActionChannelCreate(
			MTP_string(action.value("title").toString()));
	} else if (type == u"member_added"_q) {
		return MTP_messageActionChatAddUser(MTP_vector<MTPlong>(users));
	} else if (type == u"member_joined"_q) {
		// Upstream words an add by the added user himself as "joined".
		return MTP_messageActionChatAddUser(
			MTP_vector<MTPlong>(QVector<MTPlong>{ MTP_long(actorId) }));
	} else if (type == u"member_left"_q || type == u"member_removed"_q) {
		return MTP_messageActionChatDeleteUser(users.isEmpty()
			? MTP_long(actorId)
			: users.front());
	} else if (type == u"title_changed"_q) {
		return MTP_messageActionChatEditTitle(
			MTP_string(action.value("title").toString()));
	} else if (type == u"photo_changed"_q) {
		const auto removed = action.value("photo_url").toString().isEmpty();
		const auto text = channel
			? (removed
				? tr::lng_action_removed_photo_channel(tr::now)
				: tr::lng_action_changed_photo_channel(tr::now))
			: (removed
				? tr::lng_action_removed_photo(
					tr::now,
					lt_from,
					UserName(session, actorId))
				: tr::lng_action_changed_photo(
					tr::now,
					lt_from,
					UserName(session, actorId)));
		return MTP_messageActionCustomAction(MTP_string(text));
	} else if (type == u"call_started"_q) {
		return MTP_messageActionGroupCall(MTP_flags(0), call, MTPint());
	} else if (type == u"call_ended"_q) {
		return MTP_messageActionGroupCall(
			MTP_flags(MTPDmessageActionGroupCall::Flag::f_duration),
			call,
			MTP_int(action.value("duration").toInt()));
	} else if (type == u"call_invited"_q) {
		return MTP_messageActionInviteToGroupCall(
			call,
			MTP_vector<MTPlong>(users));
	} else if (type == u"call_scheduled"_q) {
		return MTP_messageActionGroupCallScheduled(
			call,
			MTP_int(int32(Number(action, "schedule_date"))));
	}
	return MTP_messageActionEmpty();
}

} // namespace CustomBackend::Community
