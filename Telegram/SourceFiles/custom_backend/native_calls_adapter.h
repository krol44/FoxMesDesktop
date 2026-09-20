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
} // namespace Main

namespace CustomBackend::Calls {

// FoxMes has no MTProto, but it has the same call: the upstream state machine
// (calls/calls_call.cpp) drives seven requests and reacts to two updates, and
// everything above that - the panel, the camera, screen sharing, muting, the
// emoji fingerprint - lives inside tgcalls and never touches a server.
//
// So this adapter substitutes the transport and nothing else. Every function
// below answers in the shape of the TL constructor upstream expects, so the
// completion lambdas in calls_call.cpp keep working unchanged; the key of the
// conversation is derived on the two clients and never reaches us.

// Answers carry the phone call exactly as upstream would receive it inside
// phone.phoneCall, and a failure carries the error string upstream already
// knows how to show (Call::handleRequestError).
using Done = std::function<void(const MTPPhoneCall &)>;
using Fail = std::function<void(const QString &)>;
using Plain = std::function<void()>;

// phone.requestCall. The chat is resolved from the user before the request,
// so completion still happens exactly once, after the server answered.
void RequestCall(
	not_null<UserData*> user,
	bytes::const_span gaHash,
	bool video,
	const MTPPhoneCallProtocol &protocol,
	Done done,
	Fail fail);

// phone.receivedCall - this device is showing the call.
void ReceivedCall(
	not_null<Main::Session*> session,
	uint64 callId,
	Plain done,
	Fail fail);

// phone.acceptCall / phone.confirmCall - the two halves of the key exchange.
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

// phone.discardCall. Upstream wants the request sent even when the call
// object is already gone, so completion is a plain callback and failure is
// reported through the same one: the caller only uses it to settle its final
// state.
void DiscardCall(
	not_null<Main::Session*> session,
	uint64 callId,
	int duration,
	const MTPPhoneCallDiscardReason &reason,
	bool video,
	Plain done);

// phone.sendSignalingData. The packet is opaque to the bridge and to the
// server; false means the server refused it, which upstream treats as a
// failed call.
void SendSignalingData(
	not_null<Main::Session*> session,
	uint64 callId,
	const QByteArray &data,
	std::function<void(bool)> done,
	Fail fail);

// messages.getDhConfig and phone.getCallConfig.
void RequestDhConfig(
	not_null<Main::Session*> session,
	std::function<void(const MTPmessages_DhConfig &)> done,
	Fail fail);
void RequestCallConfig(
	not_null<Main::Session*> session,
	std::function<void(const QByteArray &)> done,
	Fail fail);

// One finished call as the service message upstream draws it
// (history_item.cpp, MTPDmessageActionPhoneCall -> Data::MediaCall). Shared by
// the chat history and the recent calls list so both describe a call the same
// way.
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

// The recent calls list. Upstream asks for it with messages.search and the
// phone-call filter, which the bridge has nothing to answer with, so the rows
// come from the call history instead and are handed back in the same shape.
void LoadHistory(
	not_null<Main::Session*> session,
	MsgId offsetId,
	int limit,
	std::function<void(const MTPmessages_Messages &)> done,
	Plain fail);
void ClearHistory(not_null<Main::Session*> session, Plain done);

// Whether this device accepts incoming calls - the bridge's counterpart of the
// per-authorization switch upstream drives through
// account.setAuthorizationSettings. A device is a FoxMes session, so the answer
// is per install and per account, not per user.
[[nodiscard]] rpl::producer<bool> AcceptCallsValue(
	not_null<Main::Session*> session);
[[nodiscard]] bool AcceptCallsCurrent(not_null<Main::Session*> session);
void SetAcceptCalls(not_null<Main::Session*> session, bool accept);

// Live updates arrive as WebSocket events rather than MTProto updates. The
// bridge turns them back into the update upstream expects and feeds them to
// Calls::Instance, which is why the incoming half needs no upstream hook at
// all. Returns false for an event this adapter does not own.
[[nodiscard]] bool HandleEvent(
	Main::Session *session,
	const QString &type,
	const QJsonObject &data);

} // namespace CustomBackend::Calls
