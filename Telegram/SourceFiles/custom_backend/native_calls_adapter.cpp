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

[[nodiscard]] MTPPhoneCallDiscardReason ParseReason(const QString &reason) {
	if (reason == u"missed"_q) {
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
	}, [](const auto &) {
		return u"hangup"_q;
	});
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
[[nodiscard]] QString ErrorText(const QJsonDocument &doc, const QString &error) {
	const auto code = doc.isObject()
		? doc.object().value("code").toString()
		: QString();
	if (code == u"call_busy"_q) {
		return code;
	} else if (code == u"call_peer_unsupported"_q) {
		// Upstream already has a box for "their app cannot take calls", and
		// no FoxMes Desktop on the other side is exactly that case.
		return u"PARTICIPANT_VERSION_OUTDATED"_q;
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

void AnswerCall(
		const QJsonDocument &doc,
		const QString &error,
		const Done &done,
		const Fail &fail) {
	if (!error.isEmpty()) {
		if (fail) {
			fail(ErrorText(doc, error));
		}
		return;
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
	const auto &fields = protocol.c_phoneCallProtocol();
	auto versions = QJsonArray();
	for (const auto &version : fields.vlibrary_versions().v) {
		versions.push_back(QString::fromUtf8(version.v));
	}
	const auto payload = QJsonObject{
		{"min_layer", fields.vmin_layer().v},
		{"max_layer", fields.vmax_layer().v},
		{"library_versions", versions},
		{"udp_p2p", fields.is_udp_p2p()},
		{"udp_reflector", fields.is_udp_reflector()},
	};
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
			[=](QJsonDocument doc, QString error, int) {
				AnswerCall(doc, error, done, fail);
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
	client->callReceived(qint64(callId), [=](QJsonDocument doc, QString error, int) {
		if (!error.isEmpty()) {
			if (fail) {
				fail(ErrorText(doc, error));
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
	// The protocol travels with the request that created the call; both sides
	// already agreed on it, and re-stating it here would be a second source.
	client->callAccept(qint64(callId), Copy(gb), [=](QJsonDocument doc, QString error, int) {
		AnswerCall(doc, error, done, fail);
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
		[=](QJsonDocument doc, QString error, int) {
			AnswerCall(doc, error, done, fail);
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
	// Upstream settles its final state on either outcome: a hang-up the server
	// never heard still ends the call on this side.
	client->callDiscard(
		qint64(callId),
		ReasonText(reason),
		duration,
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
	client->callSignaling(qint64(callId), data, [=](QJsonDocument doc, QString error, int) {
		if (!error.isEmpty()) {
			if (fail) {
				fail(ErrorText(doc, error));
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

// Accepting calls is a property of this device, so the state is per session and
// dies with it. It starts as "accepts" and is corrected by the first answer:
// the switch has to draw something before the request comes back, and a device
// that has never been touched does accept calls.
struct AcceptCallsState {
	rpl::variable<bool> value = true;
	bool requested = false;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<AcceptCallsState>> gAcceptCalls;

[[nodiscard]] AcceptCallsState &AcceptCallsFor(not_null<Main::Session*> session) {
	auto &state = gAcceptCalls[session];
	if (!state) {
		state = std::make_unique<AcceptCallsState>();
		session->lifetime().add([session] { gAcceptCalls.remove(session); });
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
				const auto i = gAcceptCalls.find(not_null(strong));
				if (i != end(gAcceptCalls)) {
					i->second->value = doc.object()
						.value("accept_calls")
						.toBool(true);
				}
			});
		}
	}
	return *state;
}

} // namespace

rpl::producer<bool> AcceptCallsValue(not_null<Main::Session*> session) {
	return AcceptCallsFor(session).value.value();
}

bool AcceptCallsCurrent(not_null<Main::Session*> session) {
	return AcceptCallsFor(session).value.current();
}

void SetAcceptCalls(not_null<Main::Session*> session, bool accept) {
	auto &state = AcceptCallsFor(session);
	state.value = accept;
	if (const auto client = ClientOrNull(session)) {
		client->callSettingsUpdate(accept, [](QJsonDocument, QString, int) {});
	}
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
		for (const auto &entry : data.value("calls").toArray()) {
			const auto call = entry.toObject();
			const auto chatId = call.value("chat_id").toVariant().toLongLong();
			const auto history = target->historyForChat(chatId);
			const auto messageId = call.value("message_id").toVariant().toLongLong();
			if (!history || messageId <= 0 || messageId > INT32_MAX) {
				continue;
			}
			const auto outgoing = call.value("outgoing").toBool();
			messages.push_back(BuildCallMessage(
				history->peer->id,
				outgoing,
				MsgId(int32(messageId)),
				outgoing ? me : call.value("peer_id").toVariant().toLongLong(),
				Number(call, "id"),
				call.value("reason").toString(),
				call.value("duration").toInt(),
				call.value("video").toBool(),
				TimeId(call.value("date").toInt())));
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
	const auto call = ParseCall(data);
	if (!call) {
		return true;
	}
	Core::App().calls().handleUpdate(session, MTP_updatePhoneCall(*call));
	return true;
}

} // namespace CustomBackend::Calls
