/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace Main {
class Session;
}

namespace CustomBackend::Community {


[[nodiscard]] bool IsCommunity(const QJsonObject &chat);
[[nodiscard]] bool IsChannel(const QJsonObject &chat);
[[nodiscard]] qint64 OwnerId(const QJsonObject &chat);

[[nodiscard]] MTPChat Channel(
	const QJsonObject &chat,
	qint64 me,
	bool member = true);

[[nodiscard]] MTPChatFull ChannelFull(
	const QJsonObject &chat,
	qint64 me,
	const MTPPeerNotifySettings &notify,
	TimeId ttlPeriod);

[[nodiscard]] MTPChannelParticipant Participant(
	const QJsonObject &member,
	qint64 me);

[[nodiscard]] MTPChatAdminRights OwnerRights();

[[nodiscard]] MTPMessageAction Action(
	not_null<Main::Session*> session,
	const QJsonObject &action,
	qint64 actorId,
	bool channel);

}
