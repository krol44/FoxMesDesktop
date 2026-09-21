/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_calls_adapter.h"

#include "base/debug_log.h"
#include "base/weak_ptr.h"
#include "calls/calls_instance.h"
#include "core/application.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "base/flat_map.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_session.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace CustomBackend::Calls {
namespace {

// Every answer names its TL constructor in "_". Parsing branches on that and
// on nothing else: a field belongs to exactly one form, and guessing the form
// from which fields happen to be present is how the two sides drift apart.
[[nodiscard]] QByteArray Bytes(const QJsonObject &data, const char *key) {
	return QByteArray::fromBase64(data.value(QLatin1String(key)).toString().toLatin1());
}

// The handshake halves are byte spans upstream, and they have to survive the
// asynchronous hop to the request: a span points at storage the caller may
// reuse the moment it returns.
[[nodiscard]] QByteArray Copy(bytes::const_span data) {
	return QByteArray(
		reinterpret_cast<const char*>(data.data()),
		int(data.size()));
}

[[nodiscard]] qint64 Number(const QJsonObject &data, const char *key) {
	return data.value(QLatin1String(key)).toVariant().toLongLong();
}

[[nodiscard]] MTPPhoneCallProtocol ParseProtocol(const QJsonObject &data) {
	using Flag = MTPDphoneCallProtocol::Flag;
	auto flags = MTPDphoneCallProtocol::Flags();
	if (data.value("udp_p2p").toBool()) {
		flags |= Flag::f_udp_p2p;
	}
	if (data.value("udp_reflector").toBool()) {
		flags |= Flag::f_udp_reflector;
	}
	auto versions = QVector<MTPstring>();
	for (const auto &version : data.value("library_versions").toArray()) {
		versions.push_back(MTP_bytes(version.toString().toUtf8()));
	}
	return MTP_phoneCallProtocol(
		MTP_flags(flags),
		MTP_int(data.value("min_layer").toInt()),
		MTP_int(data.value("max_layer").toInt()),
		MTP_vector<MTPstring>(std::move(versions)));
}

[[nodiscard]] MTPPhoneConnection ParseConnection(const QJsonObject &data) {
	using Flag = MTPDphoneConnectionWebrtc::Flag;
	auto flags = MTPDphoneConnectionWebrtc::Flags();
	if (data.value("turn").toBool()) {
		flags |= Flag::f_turn;
	}
	if (data.value("stun").toBool()) {
		flags |= Flag::f_stun;
	}
	return MTP_phoneConnectionWebrtc(
		MTP_flags(flags),
		MTP_long(Number(data, "id")),
		MTP_string(data.value("ip").toString()),
		MTP_string(data.value("ipv6").toString()),
		MTP_int(data.value("port").toInt()),
		MTP_string(data.value("username").toString()),
		MTP_string(data.value("password").toString()));
}

[[nodiscard]] MTPPhoneCallDiscardReason ParseReason(
		const QString &reason,
		const QString &slug = QString()) {
	if (reason == u"migrate_conference_call"_q && !slug.isEmpty()) {
		// The call moved into a group call; upstream finds it by the slug and
		// joins (Call::finishByMigration).
		return MTP_phoneCallDiscardReasonMigrateConferenceCall(
			MTP_string(slug));
	} else if (reason == u"missed"_q) {
		return MTP_phoneCallDiscardReasonMissed();
	} else if (reason == u"busy"_q) {
		return MTP_phoneCallDiscardReasonBusy();
	} else if (reason == u"disconnect"_q) {
		return MTP_phoneCallDiscardReasonDisconnect();
	}
	return MTP_phoneCallDiscardReasonHangup();
}

[[nodiscard]] QString ReasonText(const MTPPhoneCallDiscardReason &reason) {
	return reason.match([](const MTPDphoneCallDiscardReasonMissed &) {
		return u"missed"_q;
	}, [](const MTPDphoneCallDiscardReasonBusy &) {
		return u"busy"_q;
	}, [](const MTPDphoneCallDiscardReasonDisconnect &) {
		return u"disconnect"_q;
	}, [](const MTPDphoneCallDiscardReasonMigrateConferenceCall &) {
		return u"migrate_conference_call"_q;
	}, [](const auto &) {
		return u"hangup"_q;
	});
}

[[nodiscard]] QString ReasonSlug(const MTPPhoneCallDiscardReason &reason) {
	return reason.match([](
			const MTPDphoneCallDiscardReasonMigrateConferenceCall &data) {
		return qs(data.vslug());
	}, [](const auto &) {
		return QString();
	});
}

// One line per call form for the log: the bridge is the only place that sees
// what the server actually answered, and upstream logs none of it.
[[nodiscard]] QString Describe(const QJsonObject &data) {
	auto versions = QStringList();
	for (const auto &version : data.value("protocol").toObject()
			.value("library_versions").toArray()) {
		versions.push_back(version.toString());
	}
	return u"%1 id=%2 reason=%3 connections=%4 p2p=%5 versions=[%6]"_q
		.arg(data.value("_").toString())
		.arg(Number(data, "id"))
		.arg(data.value("reason").toString())
		.arg(data.value("connections").toArray().size())
		.arg(data.value("p2p_allowed").toBool() ? 1 : 0)
		.arg(versions.join(','));
}

[[nodiscard]] std::optional<MTPPhoneCall> ParseCall(const QJsonObject &data) {
	const auto type = data.value("_").toString();
	const auto id = MTP_long(Number(data, "id"));
	if (type == u"phoneCallDiscarded"_q) {
		using Flag = MTPDphoneCallDiscarded::Flag;
		auto flags = MTPDphoneCallDiscarded::Flags();
		flags |= Flag::f_reason;
		flags |= Flag::f_duration;
		if (data.value("video").toBool()) {
			flags |= Flag::f_video;
		}
		// need_rating and need_debug stay off by contract: the bridge collects
		// neither, and asking would show a dialog with nowhere to send it.
		return MTP_phoneCallDiscarded(
			MTP_flags(flags),
			id,
			ParseReason(data.value("reason").toString()),
			MTP_int(data.value("duration").toInt()));
	}

	const auto accessHash = MTP_long(Number(data, "access_hash"));
	const auto date = MTP_int(data.value("date").toInt());
	const auto adminId = MTP_long(Number(data, "admin_id"));
	const auto participantId = MTP_long(Number(data, "participant_id"));
	const auto protocol = ParseProtocol(data.value("protocol").toObject());
	const auto video = data.value("video").toBool();

	if (type == u"phoneCallWaiting"_q) {
		using Flag = MTPDphoneCallWaiting::Flag;
		auto flags = MTPDphoneCallWaiting::Flags();
		if (video) {
			flags |= Flag::f_video;
		}
		const auto receiveDate = data.value("receive_date").toInt();
		if (receiveDate > 0) {
			flags |= Flag::f_receive_date;
		}
		return MTP_phoneCallWaiting(
			MTP_flags(flags),
			id,
			accessHash,
			date,
			adminId,
			participantId,
			protocol,
			MTP_int(receiveDate));
	} else if (type == u"phoneCallRequested"_q) {
		using Flag = MTPDphoneCallRequested::Flag;
		auto flags = MTPDphoneCallRequested::Flags();
		if (video) {
			flags |= Flag::f_video;
		}
		return MTP_phoneCallRequested(
			MTP_flags(flags),
			id,
			accessHash,
			date,
			adminId,
			participantId,
			MTP_bytes(Bytes(data, "g_a_hash")),
			protocol);
	} else if (type == u"phoneCallAccepted"_q) {
		using Flag = MTPDphoneCallAccepted::Flag;
		auto flags = MTPDphoneCallAccepted::Flags();
		if (video) {
			flags |= Flag::f_video;
		}
		return MTP_phoneCallAccepted(
			MTP_flags(flags),
			id,
			accessHash,
			date,
			adminId,
			participantId,
			MTP_bytes(Bytes(data, "g_b")),
			protocol);
	} else if (type == u"phoneCall"_q) {
		using Flag = MTPDphoneCall::Flag;
		auto flags = MTPDphoneCall::Flags();
		if (video) {
			flags |= Flag::f_video;
		}
		if (data.value("p2p_allowed").toBool()) {
			flags |= Flag::f_p2p_allowed;
		}
		if (data.value("conference_supported").toBool()) {
			// Shows "Add People" in the call panel: the server can take the
			// call on as a group call.
			flags |= Flag::f_conference_supported;
		}
		auto connections = QVector<MTPPhoneConnection>();
		for (const auto &connection : data.value("connections").toArray()) {
			connections.push_back(ParseConnection(connection.toObject()));
		}
		return MTP_phoneCall(
			MTP_flags(flags),
			id,
			accessHash,
			date,
			adminId,
			participantId,
			MTP_bytes(Bytes(data, "g_a_or_b")),
			MTP_long(Number(data, "key_fingerprint")),
			protocol,
			MTP_vector<MTPPhoneConnection>(std::move(connections)),
			MTP_int(data.value("start_date").toInt()),
			MTPDataJSON());
	}
	LOG(("FoxMes: unknown phone call form '%1'").arg(type));
	return std::nullopt;
}

// The bridge answers with a contract error string; upstream shows anything it
// does not recognise verbatim, so the ones it does recognise must come through
// untouched and the rest must not reach the user as server jargon.
[[nodiscard]] QString ErrorText(
		const QJsonDocument &doc,
		const QString &error,
		int status) {
	const auto code = doc.isObject()
		? doc.object().value("code").toString()
		: QString();
	if (status == 404) {
		// The call is already gone: it timed out, the other side hung up, or
		// this client outlived its own call. Upstream ends the call on an
		// empty error and says nothing - and there is nothing to say, the
		// call is over either way. A box with the server's own words here was
		// the "call not found" the user saw.
		return QString();
	} else if (code == u"call_busy"_q) {
		return code;
	} else if (code == u"call_version_mismatch"_q) {
		// No call library version the two builds share. Upstream already has
		// a box for "their app is too old", which is what this is.
		return u"PARTICIPANT_VERSION_OUTDATED"_q;
	} else if (code == u"call_peer_unsupported"_q) {
		// Upstream already has a box for "their app cannot take calls", and
		// no FoxMes Desktop on the other side is exactly that case.
		return u"PARTICIPANT_VERSION_OUTDATED"_q;
	} else if (code == u"call_peer_offline"_q) {
		// None of their devices is connected, and without push nothing would
		// ring. Upstream has no string for this; the server sends one ready
		// to show, and upstream shows an unknown error text verbatim.
		const auto message = doc.object().value("message").toString();
		return message.isEmpty() ? code : message;
	} else if (code == u"call_peer_refuses"_q) {
		// Every device of theirs has "Accept calls" switched off. Upstream
		// says this as a privacy setting, which is what the switch is.
		return u"USER_PRIVACY_RESTRICTED"_q;
	}
	return error;
}

[[nodiscard]] ApiClient *ClientOrNull(Main::Session *session) {
	return (session && Enabled()) ? &ClientFor(session) : nullptr;
}

// The protocol of this device as the server wants it. Both ends of a call send
// it: the version of the call library has to exist on both, and the desktop
// and the phone do not register the same set.
[[nodiscard]] QJsonObject ProtocolJson(const MTPPhoneCallProtocol &protocol) {
	const auto &fields = protocol.c_phoneCallProtocol();
	auto versions = QJsonArray();
	for (const auto &version : fields.vlibrary_versions().v) {
		versions.push_back(QString::fromUtf8(version.v));
	}
	return QJsonObject{
		{"min_layer", fields.vmin_layer().v},
		{"max_layer", fields.vmax_layer().v},
		{"library_versions", versions},
		{"udp_p2p", fields.is_udp_p2p()},
		{"udp_reflector", fields.is_udp_reflector()},
	};
}

void AnswerCall(
		const char *what,
		const QJsonDocument &doc,
		const QString &error,
		int status,
		const Done &done,
		const Fail &fail) {
	if (!error.isEmpty()) {
		LOG(("FoxMes Call: %1 failed, status %2, error '%3', body %4").arg(
			QString::fromLatin1(what),
			QString::number(status),
			error,
			QString::fromUtf8(doc.toJson(QJsonDocument::Compact))));
		if (fail) {
			fail(ErrorText(doc, error, status));
		}
		return;
	}
	if (doc.isObject()) {
		LOG(("FoxMes Call: %1 -> %2").arg(
			QString::fromLatin1(what),
			Describe(doc.object().value("call").toObject())));
	}
	const auto call = doc.isObject()
		? ParseCall(doc.object().value("call").toObject())
		: std::nullopt;
	if (!call) {
		if (fail) {
			fail(u"CALL_RESPONSE_INVALID"_q);
		}
		return;
	}
	if (done) {
		done(*call);
	}
}

} // namespace

void RequestCall(
		not_null<UserData*> user,
		bytes::const_span gaHash,
		bool video,
		const MTPPhoneCallProtocol &protocol,
		Done done,
		Fail fail) {
	const auto session = &user->session();
	const auto bridge = Enabled() ? BridgeFor(session) : nullptr;
	if (!bridge) {
		if (fail) {
			fail(u"CALLS_UNAVAILABLE"_q);
		}
		return;
	}
	const auto payload = ProtocolJson(protocol);
	const auto hash = Copy(gaHash);
	const auto weak = base::make_weak(session);
	// A call is placed from an open private chat, and the server needs that
	// chat: it is what proves the two may call each other at all. Resolution
	// runs first and the completion still fires exactly once, after the answer.
	bridge->resolveChatId(session->data().history(user), [=](qint64 chatId) {
		const auto strong = weak.get();
		const auto client = ClientOrNull(strong);
		if (!client) {
			return;
		}
		if (chatId <= 0) {
			if (fail) {
				fail(u"CALL_CHAT_UNRESOLVED"_q);
			}
			return;
		}
		client->requestCall(
			chatId,
			hash,
			video,
			payload,
			QUuid::createUuid().toString(QUuid::WithoutBraces),
			[=](QJsonDocument doc, QString error, int status) {
				AnswerCall("request", doc, error, status, done, fail);
			});
	});
}

void ReceivedCall(
		not_null<Main::Session*> session,
		uint64 callId,
		Plain done,
		Fail fail) {
	const auto client = ClientOrNull(session);
	if (!client) {
		return;
	}
	client->callReceived(qint64(callId), [=](QJsonDocument doc, QString error, int status) {
		if (!error.isEmpty()) {
			if (fail) {
				fail(ErrorText(doc, error, status));
			}
		} else if (done) {
			done();
		}
	});
}

void AcceptCall(
		not_null<Main::Session*> session,
		uint64 callId,
		bytes::const_span gb,
		const MTPPhoneCallProtocol &protocol,
		Done done,
		Fail fail) {
	const auto client = ClientOrNull(session);
	if (!client) {
		return;
	}
	client->callAccept(
		qint64(callId),
		Copy(gb),
		ProtocolJson(protocol),
		[=](QJsonDocument doc, QString error, int status) {
			AnswerCall("accept", doc, error, status, done, fail);
		});
}

void ConfirmCall(
		not_null<Main::Session*> session,
		uint64 callId,
		bytes::const_span ga,
		uint64 keyFingerprint,
		const MTPPhoneCallProtocol &protocol,
		Done done,
		Fail fail) {
	const auto client = ClientOrNull(session);
	if (!client) {
		return;
	}
	client->callConfirm(
		qint64(callId),
		Copy(ga),
		qint64(keyFingerprint),
		[=](QJsonDocument doc, QString error, int status) {
			AnswerCall("confirm", doc, error, status, done, fail);
		});
}

void DiscardCall(
		not_null<Main::Session*> session,
		uint64 callId,
		int duration,
		const MTPPhoneCallDiscardReason &reason,
		bool video,
		Plain done) {
	const auto client = ClientOrNull(session);
	if (!client) {
		if (done) {
			done();
		}
		return;
	}
	LOG(("FoxMes Call: discard id=%1 reason=%2 duration=%3").arg(
		QString::number(qint64(callId)),
		ReasonText(reason),
		QString::number(duration)));
	// Upstream settles its final state on either outcome: a hang-up the server
	// never heard still ends the call on this side.
	client->callDiscard(
		qint64(callId),
		ReasonText(reason),
		duration,
		ReasonSlug(reason),
		[=](QJsonDocument, QString, int) {
			if (done) {
				done();
			}
		});
}

void SendSignalingData(
		not_null<Main::Session*> session,
		uint64 callId,
		const QByteArray &data,
		std::function<void(bool)> done,
		Fail fail) {
	const auto client = ClientOrNull(session);
	if (!client) {
		return;
	}
	client->callSignaling(qint64(callId), data, [=](QJsonDocument doc, QString error, int status) {
		if (!error.isEmpty()) {
			if (fail) {
				fail(ErrorText(doc, error, status));
			}
		} else if (done) {
			done(true);
		}
	});
}

void RequestDhConfig(
		not_null<Main::Session*> session,
		std::function<void(const MTPmessages_DhConfig &)> done,
		Fail fail) {
	const auto client = ClientOrNull(session);
	if (!client) {
		return;
	}
	client->callDhConfig([=](QJsonDocument doc, QString error, int) {
		if (!error.isEmpty() || !doc.isObject()) {
			if (fail) {
				fail(error.isEmpty() ? u"DH_CONFIG_INVALID"_q : error);
			}
			return;
		}
		const auto data = doc.object();
		if (done) {
			done(MTP_messages_dhConfig(
				MTP_int(data.value("g").toInt()),
				MTP_bytes(Bytes(data, "p")),
				MTP_int(data.value("version").toInt()),
				MTP_bytes(Bytes(data, "random"))));
		}
	});
}

void RequestCallConfig(
		not_null<Main::Session*> session,
		std::function<void(const QByteArray &)> done,
		Fail fail) {
	const auto client = ClientOrNull(session);
	if (!client) {
		return;
	}
	client->callConfig([=](QJsonDocument doc, QString error, int) {
		if (!error.isEmpty() || !doc.isObject()) {
			if (fail) {
				fail(error.isEmpty() ? u"CALL_CONFIG_INVALID"_q : error);
			}
			return;
		}
		if (done) {
			done(doc.object().value("data").toString().toUtf8());
		}
	});
}

namespace {

// Both switches are properties of this device, so the state is per session and
// dies with it. The defaults are the ones the server answers for a device that
// has never been touched: it accepts calls, and it keeps media on the relay.
// They have to draw something before the first answer comes back.
struct CallDeviceState {
	rpl::variable<bool> acceptCalls = true;
	rpl::variable<bool> allowP2P = false;
	bool requested = false;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<CallDeviceState>> gCallDevice;

[[nodiscard]] CallDeviceState &CallDeviceFor(not_null<Main::Session*> session) {
	auto &state = gCallDevice[session];
	if (!state) {
		state = std::make_unique<CallDeviceState>();
		session->lifetime().add([session] { gCallDevice.remove(session); });
	}
	if (!state->requested) {
		state->requested = true;
		if (const auto client = ClientOrNull(session)) {
			const auto weak = base::make_weak(session);
			client->callSettings([=](QJsonDocument doc, QString error, int) {
				const auto strong = weak.get();
				if (!strong || !error.isEmpty() || !doc.isObject()) {
					return;
				}
				const auto i = gCallDevice.find(not_null(strong));
				if (i != end(gCallDevice)) {
					const auto data = doc.object();
					i->second->acceptCalls = data
						.value("accept_calls")
						.toBool(true);
					i->second->allowP2P = data
						.value("p2p_allowed")
						.toBool(false);
				}
			});
		}
	}
	return *state;
}

void SaveCallDevice(
		not_null<Main::Session*> session,
		const QJsonObject &settings) {
	if (const auto client = ClientOrNull(session)) {
		client->callSettingsUpdate(
			settings,
			[](QJsonDocument, QString, int) {});
	}
}

} // namespace

rpl::producer<bool> AcceptCallsValue(not_null<Main::Session*> session) {
	return CallDeviceFor(session).acceptCalls.value();
}

bool AcceptCallsCurrent(not_null<Main::Session*> session) {
	return CallDeviceFor(session).acceptCalls.current();
}

void SetAcceptCalls(not_null<Main::Session*> session, bool accept) {
	CallDeviceFor(session).acceptCalls = accept;
	SaveCallDevice(session, QJsonObject{ { "accept_calls", accept } });
}

rpl::producer<bool> AllowP2PValue(not_null<Main::Session*> session) {
	return CallDeviceFor(session).allowP2P.value();
}

bool AllowP2PCurrent(not_null<Main::Session*> session) {
	return CallDeviceFor(session).allowP2P.current();
}

void SetAllowP2P(not_null<Main::Session*> session, bool allow) {
	CallDeviceFor(session).allowP2P = allow;
	SaveCallDevice(session, QJsonObject{ { "p2p_allowed", allow } });
}

MTPMessage BuildCallMessage(
		PeerId peerId,
		bool out,
		MsgId messageId,
		qint64 senderId,
		qint64 callId,
		const QString &reason,
		int duration,
		bool video,
		TimeId date) {
	using Flag = MTPDmessageService::Flag;
	auto flags = Flag::f_from_id | Flag();
	if (out) {
		flags |= Flag::f_out;
	}
	using ActionFlag = MTPDmessageActionPhoneCall::Flag;
	auto actionFlags = ActionFlag::f_reason | ActionFlag::f_duration;
	if (video) {
		actionFlags |= ActionFlag::f_video;
	}
	return MTP_messageService(
		MTP_flags(flags),
		MTP_int(messageId.bare),
		MTP_peerUser(MTP_long(senderId)),
		peerToMTP(peerId),
		MTPPeer(), // saved_peer_id
		MTPMessageReplyHeader(),
		MTP_int(date),
		MTP_messageActionPhoneCall(
			MTP_flags(actionFlags),
			MTP_long(callId),
			ParseReason(reason),
			MTP_int(duration)),
		MTPMessageReactions(),
		MTPint()); // ttl_period
}

MTPMessage BuildConferenceMessage(
		PeerId peerId,
		bool out,
		MsgId messageId,
		qint64 senderId,
		const QJsonObject &call,
		TimeId date) {
	using Flag = MTPDmessageService::Flag;
	auto flags = Flag::f_from_id | Flag();
	if (out) {
		flags |= Flag::f_out;
	}
	using ActionFlag = MTPDmessageActionConferenceCall::Flag;
	auto actionFlags = MTPDmessageActionConferenceCall::Flags();
	const auto state = call.value("state").toString();
	const auto duration = call.value("duration").toInt();
	if (state == u"missed"_q) {
		actionFlags |= ActionFlag::f_missed;
	} else if (state == u"active"_q) {
		actionFlags |= ActionFlag::f_active;
	} else if (state == u"ended"_q && duration > 0) {
		actionFlags |= ActionFlag::f_duration;
	}
	if (call.value("video").toBool()) {
		actionFlags |= ActionFlag::f_video;
	}
	auto participants = QVector<MTPPeer>();
	for (const auto &id : call.value("participants").toArray()) {
		const auto userId = id.toVariant().toLongLong();
		if (userId > 0) {
			participants.push_back(MTP_peerUser(MTP_long(userId)));
		}
	}
	if (!participants.isEmpty()) {
		actionFlags |= ActionFlag::f_other_participants;
	}
	return MTP_messageService(
		MTP_flags(flags),
		MTP_int(messageId.bare),
		MTP_peerUser(MTP_long(senderId)),
		peerToMTP(peerId),
		MTPPeer(), // saved_peer_id
		MTPMessageReplyHeader(),
		MTP_int(date),
		MTP_messageActionConferenceCall(
			MTP_flags(actionFlags),
			MTP_long(Number(call, "call_id")),
			MTP_int(duration),
			MTP_vector<MTPPeer>(std::move(participants))),
		MTPMessageReactions(),
		MTPint()); // ttl_period
}

void LoadHistory(
		not_null<Main::Session*> session,
		MsgId offsetId,
		int limit,
		std::function<void(const MTPmessages_Messages &)> done,
		Plain fail) {
	const auto client = ClientOrNull(session);
	const auto bridge = Enabled() ? BridgeFor(session) : nullptr;
	if (!client || !bridge) {
		if (fail) {
			fail();
		}
		return;
	}
	const auto weak = base::make_weak(session);
	client->callHistory(offsetId.bare, limit, [=](QJsonDocument doc, QString error, int) {
		const auto strong = weak.get();
		const auto target = strong ? BridgeFor(strong) : nullptr;
		if (!target || !error.isEmpty() || !doc.isObject()) {
			if (fail) {
				fail();
			}
			return;
		}
		const auto data = doc.object();
		for (const auto &user : data.value("users").toArray()) {
			target->ensureUser(user.toObject(), true);
		}
		const auto me = client->meId();
		auto messages = QVector<MTPMessage>();
		auto skipped = 0;
		for (const auto &entry : data.value("calls").toArray()) {
			const auto call = entry.toObject();
			const auto peerId = call.value("peer_id").toVariant().toLongLong();
			const auto messageId = call.value("message_id").toVariant().toLongLong();
			if (peerId <= 0 || messageId <= 0 || messageId > INT32_MAX) {
				++skipped;
				continue;
			}
			// The row is addressed by the other participant, not by the chat:
			// a call with somebody whose dialog has not been loaded yet has no
			// history object, and looking the chat up first dropped the row
			// silently.
			const auto peer = peerFromUser(UserId(peerId));
			const auto outgoing = call.value("outgoing").toBool();
			messages.push_back(BuildCallMessage(
				peer,
				outgoing,
				MsgId(int32(messageId)),
				outgoing ? me : call.value("peer_id").toVariant().toLongLong(),
				Number(call, "id"),
				call.value("reason").toString(),
				call.value("duration").toInt(),
				call.value("video").toBool(),
				TimeId(call.value("date").toInt())));
		}
		if (skipped > 0) {
			LOG(("FoxMes: %1 of %2 calls have no message to show.").arg(
				skipped).arg(data.value("calls").toArray().size()));
		}
		const auto complete = (messages.size() < limit);
		const auto count = int(messages.size());
		auto result = complete
			? MTP_messages_messages(
				MTP_vector<MTPMessage>(std::move(messages)),
				MTP_vector<MTPForumTopic>(),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>())
			: MTP_messages_messagesSlice(
				MTP_flags(0),
				MTP_int(count),
				MTPint(), // next_rate
				MTPint(), // offset_id_offset
				MTPSearchPostsFlood(),
				MTP_vector<MTPMessage>(std::move(messages)),
				MTP_vector<MTPForumTopic>(),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>());
		if (done) {
			done(result);
		}
	});
}

void ClearHistory(not_null<Main::Session*> session, Plain done) {
	const auto client = ClientOrNull(session);
	if (!client) {
		if (done) {
			done();
		}
		return;
	}
	client->callHistoryClear([=](QJsonDocument, QString, int) {
		if (done) {
			done();
		}
	});
}

bool HandleEvent(
		Main::Session *session,
		const QString &type,
		const QJsonObject &data) {
	if (!session || !type.startsWith(u"call."_q)) {
		return false;
	}
	// Signaling is the one event that is not a call state: it carries an
	// opaque tgcalls packet addressed to the call, not a new form of it.
	if (type == u"call.signaling"_q) {
		Core::App().calls().handleUpdate(
			session,
			MTP_updatePhoneCallSignalingData(
				MTP_long(Number(data, "call_id")),
				MTP_bytes(Bytes(data, "data"))));
		return true;
	}
	LOG(("FoxMes Call: event %1 %2").arg(type, Describe(data)));
	const auto call = ParseCall(data);
	if (!call) {
		return true;
	}
	Core::App().calls().handleUpdate(session, MTP_updatePhoneCall(*call));
	return true;
}

} // namespace CustomBackend::Calls
