/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Main {
class Session;
} // namespace Main

namespace CustomBackend::Community {

// Groups and channels of fxl-api (type "group" / "channel" in the chat DTO)
// become upstream megagroups and broadcast channels. The DTO is turned into
// the TL objects upstream would have received from a server, and upstream's
// own code applies them - processChat for the channel, ApplyChannelUpdate
// for its full info - so the flags, rights and counters behave exactly as
// they do there. The creator is the only admin and holds every right.

[[nodiscard]] bool IsCommunity(const QJsonObject &chat);
[[nodiscard]] bool IsChannel(const QJsonObject &chat);
[[nodiscard]] qint64 OwnerId(const QJsonObject &chat);

// member == false builds the preview a non-member gets for a public chat:
// upstream shows it with a "Join" button.
[[nodiscard]] MTPChat Channel(
	const QJsonObject &chat,
	qint64 me,
	bool member = true);

// channelFull as a server would send it. pts is 1 on purpose: with the
// channel's own pts still 0, ApplyChannelUpdate takes the "ask for the
// dialog" branch, which the bridge answers with nothing, and leaves the read
// state the bridge keeps alone.
[[nodiscard]] MTPChatFull ChannelFull(
	const QJsonObject &chat,
	qint64 me,
	const MTPPeerNotifySettings &notify,
	TimeId ttlPeriod);

[[nodiscard]] MTPChannelParticipant Participant(
	const QJsonObject &member,
	qint64 me);

[[nodiscard]] MTPChatAdminRights OwnerRights();

// The service message of a group or channel ("action" in the message DTO).
// Photo changes come as a custom action carrying upstream's own text:
// messageActionChatEditPhoto would make upstream set the photo from the TL
// object and drop the url userpic the bridge keeps.
[[nodiscard]] MTPMessageAction Action(
	not_null<Main::Session*> session,
	const QJsonObject &action,
	qint64 actorId,
	bool channel);

} // namespace CustomBackend::Community
