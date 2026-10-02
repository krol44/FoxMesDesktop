/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "base/basic_types.h"
#include "base/bytes.h"

#include <rpl/producer.h>

#include <functional>

class UserData;

namespace Main {
class Session;
}

namespace CustomBackend::Calls {

using Done = std::function<void(const MTPPhoneCall &)>;
using Fail = std::function<void(const QString &)>;
using Plain = std::function<void()>;

void RequestCall(
	not_null<UserData*> user,
	bytes::const_span gaHash,
	bool video,
	const MTPPhoneCallProtocol &protocol,
	Done done,
	Fail fail);

void ReceivedCall(
	not_null<Main::Session*> session,
	uint64 callId,
	Plain done,
	Fail fail);

void AcceptCall(
	not_null<Main::Session*> session,
	uint64 callId,
	bytes::const_span gb,
	const MTPPhoneCallProtocol &protocol,
	Done done,
	Fail fail);
void ConfirmCall(
	not_null<Main::Session*> session,
	uint64 callId,
	bytes::const_span ga,
	uint64 keyFingerprint,
	const MTPPhoneCallProtocol &protocol,
	Done done,
	Fail fail);

void HeartbeatCall(not_null<Main::Session*> session, uint64 callId);

void DiscardCall(
	not_null<Main::Session*> session,
	uint64 callId,
	int duration,
	const MTPPhoneCallDiscardReason &reason,
	bool video,
	Plain done);

void SendSignalingData(
	not_null<Main::Session*> session,
	uint64 callId,
	const QByteArray &data,
	std::function<void(bool)> done,
	Fail fail);

void RequestDhConfig(
	not_null<Main::Session*> session,
	std::function<void(const MTPmessages_DhConfig &)> done,
	Fail fail);
void RequestCallConfig(
	not_null<Main::Session*> session,
	std::function<void(const QByteArray &)> done,
	Fail fail);

[[nodiscard]] MTPMessage BuildCallMessage(
	PeerId peerId,
	bool out,
	MsgId messageId,
	qint64 senderId,
	qint64 callId,
	const QString &reason,
	int duration,
	bool video,
	TimeId date);

[[nodiscard]] MTPMessage BuildConferenceMessage(
	PeerId peerId,
	bool out,
	MsgId messageId,
	qint64 senderId,
	const QJsonObject &call,
	TimeId date);

void LoadHistory(
	not_null<Main::Session*> session,
	MsgId offsetId,
	int limit,
	std::function<void(const MTPmessages_Messages &)> done,
	Plain fail);
void ClearHistory(not_null<Main::Session*> session, Plain done);

[[nodiscard]] rpl::producer<bool> AcceptCallsValue(
	not_null<Main::Session*> session);
[[nodiscard]] bool AcceptCallsCurrent(not_null<Main::Session*> session);
void SetAcceptCalls(not_null<Main::Session*> session, bool accept);

[[nodiscard]] rpl::producer<bool> AllowP2PValue(
	not_null<Main::Session*> session);
[[nodiscard]] bool AllowP2PCurrent(not_null<Main::Session*> session);
void SetAllowP2P(not_null<Main::Session*> session, bool allow);

[[nodiscard]] bool HandleEvent(
	Main::Session *session,
	const QString &type,
	const QJsonObject &data);

}
