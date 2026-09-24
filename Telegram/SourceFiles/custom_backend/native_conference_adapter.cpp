/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_conference_adapter.h"

#include "api/api_updates.h"
#include "apiwrap.h"
#include "base/debug_log.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_runtime.h"
#include "data/data_channel.h"
#include "data/data_group_call.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>

namespace CustomBackend::Conferences {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;
using Callback = ApiClient::Callback;
using Mtp::Answer;
using Mtp::Reader;

[[nodiscard]] qint64 Number(const QJsonObject &data, const char *key) {
	return data.value(QLatin1String(key)).toVariant().toLongLong();
}

[[nodiscard]] QByteArray Base64(const QJsonValue &value) {
	return QByteArray::fromBase64(value.toString().toLatin1());
}

[[nodiscard]] QString Base64(const QByteArray &bytes) {
	return QString::fromLatin1(bytes.toBase64());
}

[[nodiscard]] QString Base64(const MTPbytes &bytes) {
	return Base64(bytes.v);
}

// The tde2e public key as its 32 bytes, in wire order: the server only
// stores it, and taking it through the writer keeps the layout of MTPint256
// out of the picture.
[[nodiscard]] QByteArray KeyBytes(const MTPint256 &key) {
	auto buffer = mtpBuffer();
	key.write(buffer);
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		int(buffer.size() * sizeof(mtpPrime)));
}

// What a request names as the call. Upstream mostly knows the id, but the
// first look at a call can come by the slug of a 1:1 call that moved into it
// or by the invitation message.
struct Target {
	qint64 id = 0;
	qint64 accessHash = 0;
	QString slug;
	qint64 inviteMessage = 0;
};

[[nodiscard]] Target ReadTarget(const MTPInputGroupCall &call) {
	return call.match([](const MTPDinputGroupCall &data) {
		return Target{ .id = qint64(data.vid().v), .accessHash = qint64(data.vaccess_hash().v) };
	}, [](const MTPDinputGroupCallSlug &data) {
		return Target{ .slug = qs(data.vslug()) };
	}, [](const MTPDinputGroupCallInviteMessage &data) {
		return Target{ .inviteMessage = qint64(data.vmsg_id().v) };
	});
}

[[nodiscard]] qint64 ReadUserId(const MTPInputPeer &peer, qint64 me) {
	return peer.match([](const MTPDinputPeerUser &data) {
		return qint64(data.vuser_id().v);
	}, [&](const MTPDinputPeerSelf &) {
		return me;
	}, [](const auto &) {
		return qint64(0);
	});
}

[[nodiscard]] qint64 ReadUserId(const MTPInputUser &user, qint64 me) {
	return user.match([](const MTPDinputUser &data) {
		return qint64(data.vuser_id().v);
	}, [&](const MTPDinputUserSelf &) {
		return me;
	}, [](const auto &) {
		return qint64(0);
	});
}

// fxl-api answers errors with the MTProto error type in "code", so upstream
// gets exactly the error it knows how to react to (rejoin, refresh the chain
// top, "already in the call").
[[nodiscard]] QString ErrorType(
		const QJsonDocument &doc,
		const QString &error,
		int status) {
	return Mtp::ErrorType(
		doc,
		error,
		status,
		u"GROUPCALL_INVALID"_q,
		u"GROUPCALL_FORBIDDEN"_q);
}

[[nodiscard]] MTPInputGroupCall InputCall(qint64 id, qint64 accessHash) {
	return MTP_inputGroupCall(MTP_long(id), MTP_long(accessHash));
}

[[nodiscard]] MTPInputGroupCall InputCall(const QJsonObject &call) {
	return InputCall(Number(call, "id"), Number(call, "access_hash"));
}

[[nodiscard]] MTPGroupCall ParseGroupCall(const QJsonObject &data) {
	if (data.value("discarded").toBool()) {
		return MTP_groupCallDiscarded(
			MTP_long(Number(data, "id")),
			MTP_long(Number(data, "access_hash")),
			MTP_int(data.value("duration").toInt()));
	}
	using Flag = MTPDgroupCall::Flag;
	// The video chat of a group or channel is upstream's peer call, not a
	// conference: fxl-api marks it by the chat it belongs to.
	const auto chatCall = data.contains("chat_id");
	auto flags = chatCall ? MTPDgroupCall::Flags() : (Flag::f_conference | Flag());
	const auto set = [&](const char *key, Flag flag) {
		if (data.value(QLatin1String(key)).toBool()) {
			flags |= flag;
		}
	};
	set("join_muted", Flag::f_join_muted);
	set("can_change_join_muted", Flag::f_can_change_join_muted);
	set("join_date_asc", Flag::f_join_date_asc);
	set("can_start_video", Flag::f_can_start_video);
	set("creator", Flag::f_creator);
	set("messages_enabled", Flag::f_messages_enabled);
	set("can_change_messages_enabled", Flag::f_can_change_messages_enabled);
	set("schedule_start_subscribed", Flag::f_schedule_start_subscribed);
	const auto title = data.value("title").toString();
	if (!title.isEmpty()) {
		flags |= Flag::f_title;
	}
	const auto scheduleDate = data.value("schedule_date").toInt();
	if (scheduleDate) {
		flags |= Flag::f_schedule_date;
	}
	const auto link = data.value("invite_link").toString();
	if (!link.isEmpty()) {
		flags |= Flag::f_invite_link;
	}
	return MTP_groupCall(
		MTP_flags(flags),
		MTP_long(Number(data, "id")),
		MTP_long(Number(data, "access_hash")),
		MTP_int(data.value("participants_count").toInt()),
		MTP_string(title),
		MTPint(), // stream_dc_id
		MTPint(), // record_start_date
		MTP_int(scheduleDate),
		MTPint(), // unmuted_video_count
		MTP_int(data.value("unmuted_video_limit").toInt()),
		MTP_int(data.value("version").toInt()),
		MTP_string(link),
		MTPlong(), // send_paid_messages_stars
		MTPPeer()); // default_send_as
}

// updateGroupCall names the chat of a group or channel video chat, which is
// what ties the call to its peer on the receiving side.
[[nodiscard]] MTPUpdate CallUpdate(const QJsonObject &call) {
	const auto chatId = Number(call, "chat_id");
	return MTP_updateGroupCall(
		MTP_flags(chatId
			? MTPDupdateGroupCall::Flag::f_peer
			: MTPDupdateGroupCall::Flags(0)),
		chatId ? MTP_peerChannel(MTP_long(chatId)) : MTPPeer(),
		ParseGroupCall(call));
}

[[nodiscard]] MTPGroupCallParticipantVideo ParseVideo(const QJsonObject &data) {
	using Flag = MTPDgroupCallParticipantVideo::Flag;
	auto flags = MTPDgroupCallParticipantVideo::Flags();
	if (data.value("paused").toBool()) {
		flags |= Flag::f_paused;
	}
	const auto audio = data.value("audio_source").toVariant().toLongLong();
	if (audio) {
		flags |= Flag::f_audio_source;
	}
	auto groups = QVector<MTPGroupCallParticipantVideoSourceGroup>();
	for (const auto &entry : data.value("source_groups").toArray()) {
		const auto group = entry.toObject();
		auto sources = QVector<MTPint>();
		for (const auto &source : group.value("sources").toArray()) {
			sources.push_back(MTP_int(int32(source.toVariant().toLongLong())));
		}
		groups.push_back(MTP_groupCallParticipantVideoSourceGroup(
			MTP_string(group.value("semantics").toString()),
			MTP_vector<MTPint>(std::move(sources))));
	}
	return MTP_groupCallParticipantVideo(
		MTP_flags(flags),
		MTP_string(data.value("endpoint").toString()),
		MTP_vector<MTPGroupCallParticipantVideoSourceGroup>(std::move(groups)),
		MTP_int(int32(audio)));
}

[[nodiscard]] MTPGroupCallParticipant ParseParticipant(const QJsonObject &data) {
	using Flag = MTPDgroupCallParticipant::Flag;
	auto flags = MTPDgroupCallParticipant::Flags();
	const auto set = [&](const char *key, Flag flag) {
		if (data.value(QLatin1String(key)).toBool()) {
			flags |= flag;
		}
	};
	set("muted", Flag::f_muted);
	set("left", Flag::f_left);
	set("can_self_unmute", Flag::f_can_self_unmute);
	set("just_joined", Flag::f_just_joined);
	set("versioned", Flag::f_versioned);
	set("self", Flag::f_self);
	set("video_joined", Flag::f_video_joined);
	const auto activeDate = data.value("active_date").toInt();
	if (activeDate) {
		flags |= Flag::f_active_date;
	}
	const auto volume = data.value("volume").toInt();
	if (volume) {
		flags |= Flag::f_volume;
	}
	const auto raiseHand = Number(data, "raise_hand_rating");
	if (raiseHand) {
		flags |= Flag::f_raise_hand_rating;
	}
	const auto video = data.value("video").toObject();
	if (!video.isEmpty()) {
		flags |= Flag::f_video;
	}
	const auto presentation = data.value("presentation").toObject();
	if (!presentation.isEmpty()) {
		flags |= Flag::f_presentation;
	}
	return MTP_groupCallParticipant(
		MTP_flags(flags),
		MTP_peerUser(MTP_long(Number(data, "user_id"))),
		MTP_int(data.value("date").toInt()),
		MTP_int(activeDate),
		MTP_int(int32(data.value("source").toVariant().toLongLong())),
		MTP_int(volume),
		MTPstring(), // about
		MTP_long(raiseHand),
		video.isEmpty() ? MTPGroupCallParticipantVideo() : ParseVideo(video),
		(presentation.isEmpty()
			? MTPGroupCallParticipantVideo()
			: ParseVideo(presentation)),
		MTPlong()); // paid_stars_total
}

[[nodiscard]] MTPVector<MTPGroupCallParticipant> ParseParticipants(
		const QJsonArray &list) {
	auto result = QVector<MTPGroupCallParticipant>();
	result.reserve(list.size());
	for (const auto &entry : list) {
		result.push_back(ParseParticipant(entry.toObject()));
	}
	return MTP_vector<MTPGroupCallParticipant>(std::move(result));
}

[[nodiscard]] MTPVector<MTPbytes> ParseBlocks(const QJsonArray &list) {
	auto result = QVector<MTPbytes>();
	result.reserve(list.size());
	for (const auto &entry : list) {
		result.push_back(MTP_bytes(Base64(entry)));
	}
	return MTP_vector<MTPbytes>(std::move(result));
}

[[nodiscard]] MTPUpdates Updates(QVector<MTPUpdate> list) {
	return MTP_updates(
		MTP_vector<MTPUpdate>(std::move(list)),
		MTP_vector<MTPUser>(),
		MTP_vector<MTPChat>(),
		MTP_int(base::unixtime::now()),
		MTP_int(0));
}

// Participants come with their users: upstream shows a participant it has
// no user for as a nameless deleted account.
void EnsureUsers(Main::Session *session, const QJsonObject &data) {
	const auto bridge = session ? BridgeFor(session) : nullptr;
	if (!bridge) {
		return;
	}
	for (const auto &user : data.value("users").toArray()) {
		bridge->ensureUser(user.toObject(), false);
	}
}

// create and join answer with everything joinDone applies: the call, this
// device's transport on the SFU, the participant list and the chain block the
// join just wrote (the client applies only blocks it got from the server,
// its own included).
[[nodiscard]] MTPUpdates JoinUpdates(const QJsonObject &data) {
	const auto call = data.value("call").toObject();
	const auto input = InputCall(call);
	auto list = QVector<MTPUpdate>();
	list.push_back(CallUpdate(call));
	list.push_back(MTP_updateGroupCallConnection(
		MTP_flags(0),
		MTP_dataJSON(MTP_bytes(data.value("params").toString().toUtf8()))));
	list.push_back(MTP_updateGroupCallParticipants(
		input,
		ParseParticipants(data.value("participants").toArray()),
		MTP_int(data.value("version").toInt())));
	// Both sub-chains, as the server of upstream answers: 0 carries the block
	// this join wrote, 1 only says where the broadcast messages start for
	// this participant. Without the second the first broadcast block looked
	// like a gap from zero, and the whole broadcast history of the call was
	// fetched again, blocks addressed to earlier participants included.
	for (const auto key : { "chain_blocks", "broadcast_blocks" }) {
		const auto blocks = data.value(QLatin1String(key)).toObject();
		if (blocks.isEmpty()) {
			continue;
		}
		list.push_back(MTP_updateGroupCallChainBlocks(
			input,
			MTP_int(blocks.value("sub_chain_id").toInt()),
			ParseBlocks(blocks.value("blocks").toArray()),
			MTP_int(blocks.value("next_offset").toInt())));
	}
	return Updates(std::move(list));
}

// --- the requests ------------------------------------------------------

struct Context {
	Main::Session *session = nullptr;
	ApiClient *client = nullptr;
	Answer answer;
	qint64 me = 0;
};

// Runs a REST call and turns a failure into the MTProto error upstream reacts
// to. The success handler gets the answer object.
void Request(
		const Context &context,
		const QByteArray &method,
		const QString &path,
		const QJsonObject &body,
		Fn<void(QJsonObject)> done) {
	const auto answer = context.answer;
	const auto session = base::make_weak(context.session);
	context.client->conferenceRequest(method, path, body, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		if (!error.isEmpty()) {
			const auto type = ErrorType(doc, error, status);
			LOG(("FoxMes Conference: %1 %2 failed, status %3, error %4").arg(
				QString::fromLatin1(method),
				path,
				QString::number(status),
				type));
			answer.fail(type, (status >= 400 && status < 600) ? status : 400);
			return;
		}
		const auto data = doc.object();
		if (const auto strong = session.get()) {
			EnsureUsers(strong, data);
		}
		done(data);
	});
}

// Turns whatever the request named into an id and an access hash. The slug
// and the invitation forms cost one extra read: fxl-api addresses a call by
// its id.
// Once a conference is created upstream names it by its link slug
// (GroupCall::inputCallSafe), so every per-call request may come by slug.
// The slug of a call never changes: remember the id it resolves to.
[[nodiscard]] QHash<QString, std::pair<qint64, qint64>> &SlugCache() {
	static auto result = QHash<QString, std::pair<qint64, qint64>>();
	return result;
}

void Resolve(
		const Context &context,
		const Target &target,
		Fn<void(qint64 id, qint64 accessHash)> done) {
	if (target.id) {
		done(target.id, target.accessHash);
		return;
	} else if (!target.slug.isEmpty()) {
		const auto i = SlugCache().constFind(target.slug);
		if (i != SlugCache().cend()) {
			done(i->first, i->second);
			return;
		}
	}
	const auto slug = target.slug;
	const auto path = !target.slug.isEmpty()
		? u"/conferences/by-slug/%1"_q.arg(
			QString::fromLatin1(QUrl::toPercentEncoding(target.slug)))
		: u"/conferences/by-invite/%1"_q.arg(target.inviteMessage);
	Request(context, "GET", path, {}, [=](QJsonObject data) {
		const auto call = data.value("call").toObject();
		const auto id = Number(call, "id");
		const auto accessHash = Number(call, "access_hash");
		if (!slug.isEmpty() && id) {
			SlugCache().insert(slug, { id, accessHash });
		}
		done(id, accessHash);
	});
}

[[nodiscard]] QString CallPath(qint64 id, const QString &tail = QString()) {
	return u"/conferences/%1%2"_q.arg(id).arg(tail);
}

void CreateConference(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	[[maybe_unused]] const auto randomId = reader.read<MTPint>();
	const auto join = (flags & (1 << 3)) != 0;
	auto body = QJsonObject{
		{ "muted", (flags & (1 << 0)) != 0 },
		{ "video_stopped", (flags & (1 << 2)) != 0 },
	};
	if (join) {
		const auto key = reader.read<MTPint256>();
		const auto block = reader.read<MTPbytes>();
		const auto params = reader.read<MTPDataJSON>();
		body.insert("public_key", Base64(KeyBytes(key)));
		body.insert("block", Base64(block));
		body.insert("params", QString::fromUtf8(params.c_dataJSON().vdata().v));
	}
	if (!reader.ok()) {
		context.answer.fail(u"REQUEST_PARSE_FAILED"_q);
		return;
	} else if (!join) {
		// A call made only to share its link. Links are not offered here, so
		// nothing upstream shows would send this; answering keeps a stray
		// request from hanging.
		context.answer.fail(u"GROUPCALL_LINK_UNSUPPORTED"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "POST", u"/conferences"_q, body, [=](QJsonObject data) {
		answer.done(JoinUpdates(data));
	});
}

void JoinConference(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	[[maybe_unused]] const auto joinAs = reader.read<MTPInputPeer>();
	if (flags & (1 << 1)) {
		[[maybe_unused]] const auto hash = reader.read<MTPstring>();
	}
	auto body = QJsonObject{
		{ "muted", (flags & (1 << 0)) != 0 },
		{ "video_stopped", (flags & (1 << 2)) != 0 },
	};
	if (flags & (1 << 3)) {
		const auto key = reader.read<MTPint256>();
		const auto block = reader.read<MTPbytes>();
		body.insert("public_key", Base64(KeyBytes(key)));
		body.insert("block", Base64(block));
	}
	const auto params = reader.read<MTPDataJSON>();
	if (!reader.ok()) {
		context.answer.fail(u"REQUEST_PARSE_FAILED"_q);
		return;
	}
	body.insert("params", QString::fromUtf8(params.c_dataJSON().vdata().v));
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		auto full = body;
		full.insert("access_hash", QString::number(accessHash));
		Request(context, "POST", CallPath(id, u"/join"_q), full, [=](
				QJsonObject data) {
			answer.done(JoinUpdates(data));
		});
	});
}

void LeaveConference(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto source = reader.read<MTPint>();
	if (!reader.ok()) {
		context.answer.done(Updates({}));
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/leave"_q), {
			{ "source", source.v },
		}, [=](QJsonObject) {
			answer.done(Updates({}));
		});
	});
}

void DiscardConference(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/discard"_q), {}, [=](
				QJsonObject data) {
			answer.done(Updates({ CallUpdate(data.value("call").toObject()) }));
		});
	});
}

void GetConference(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	[[maybe_unused]] const auto limit = reader.read<MTPint>();
	if (!reader.ok()) {
		context.answer.fail(u"REQUEST_PARSE_FAILED"_q);
		return;
	}
	const auto path = target.id
		? CallPath(target.id)
		: !target.slug.isEmpty()
		? u"/conferences/by-slug/%1"_q.arg(
			QString::fromLatin1(QUrl::toPercentEncoding(target.slug)))
		: u"/conferences/by-invite/%1"_q.arg(target.inviteMessage);
	const auto answer = context.answer;
	Request(context, "GET", path, {}, [=](QJsonObject data) {
		answer.done(MTPphone_GroupCall(MTP_phone_groupCall(
			ParseGroupCall(data.value("call").toObject()),
			ParseParticipants(data.value("participants").toArray()),
			MTP_string(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>())));
	});
}

void GetParticipants(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto ids = reader.read<MTPVector<MTPInputPeer>>();
	const auto sources = reader.read<MTPVector<MTPint>>();
	[[maybe_unused]] const auto offset = reader.read<MTPstring>();
	const auto limit = reader.read<MTPint>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	auto idList = QJsonArray();
	for (const auto &peer : ids.v) {
		if (const auto id = ReadUserId(peer, context.me)) {
			idList.push_back(id);
		}
	}
	auto sourceList = QJsonArray();
	for (const auto &source : sources.v) {
		sourceList.push_back(source.v);
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/participants"_q), {
			{ "ids", idList },
			{ "sources", sourceList },
			{ "offset", QString() },
			{ "limit", limit.v },
		}, [=](QJsonObject data) {
			// One page is the whole list: a call holds a few dozen people at
			// most, and an empty next_offset tells upstream it has them all.
			answer.done(MTPphone_GroupParticipants(MTP_phone_groupParticipants(
				MTP_int(data.value("count").toInt()),
				ParseParticipants(data.value("participants").toArray()),
				MTP_string(),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>(),
				MTP_int(data.value("version").toInt()))));
		});
	});
}

void CheckConference(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto sources = reader.read<MTPVector<MTPint>>();
	if (!reader.ok()) {
		context.answer.done(MTPVector<MTPint>(MTP_vector<MTPint>()));
		return;
	}
	auto list = QJsonArray();
	for (const auto &source : sources.v) {
		list.push_back(source.v);
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/check"_q), {
			{ "sources", list },
		}, [=](QJsonObject data) {
			auto alive = QVector<MTPint>();
			for (const auto &source : data.value("sources").toArray()) {
				alive.push_back(MTP_int(int32(source.toVariant().toLongLong())));
			}
			answer.done(MTPVector<MTPint>(MTP_vector<MTPint>(std::move(alive))));
		});
	});
}

[[nodiscard]] MTPUpdates ParticipantsUpdates(
		const MTPInputGroupCall &input,
		const QJsonObject &data) {
	return Updates({ MTP_updateGroupCallParticipants(
		input,
		ParseParticipants(data.value("participants").toArray()),
		MTP_int(data.value("version").toInt())) });
}

void EditParticipant(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto call = reader.read<MTPInputGroupCall>();
	const auto target = ReadTarget(call);
	const auto participant = reader.read<MTPInputPeer>();
	auto body = QJsonObject{
		{ "user_id", ReadUserId(participant, context.me) },
	};
	const auto readBool = [&](int bit, const char *key) {
		if (flags & (1 << bit)) {
			body.insert(QLatin1String(key), mtpIsTrue(reader.read<MTPBool>()));
		}
	};
	readBool(0, "muted");
	if (flags & (1 << 1)) {
		body.insert("volume", reader.read<MTPint>().v);
	}
	readBool(2, "raise_hand");
	readBool(3, "video_stopped");
	readBool(4, "video_paused");
	readBool(5, "presentation_paused");
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/participants/edit"_q), body, [=](
				QJsonObject data) {
			answer.done(ParticipantsUpdates(InputCall(id, accessHash), data));
		});
	});
}

void ToggleSettings(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	auto body = QJsonObject();
	if (flags & (1 << 0)) {
		body.insert("join_muted", mtpIsTrue(reader.read<MTPBool>()));
	}
	if (flags & (1 << 2)) {
		body.insert("messages_enabled", mtpIsTrue(reader.read<MTPBool>()));
	}
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/settings"_q), body, [=](
				QJsonObject data) {
			answer.done(Updates({ CallUpdate(data.value("call").toObject()) }));
		});
	});
}

void JoinPresentation(const Context &context, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	const auto target = ReadTarget(call);
	const auto params = reader.read<MTPDataJSON>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/presentation"_q), {
			{ "params", QString::fromUtf8(params.c_dataJSON().vdata().v) },
		}, [=](QJsonObject data) {
			using Flag = MTPDupdateGroupCallConnection::Flag;
			answer.done(Updates({
				MTP_updateGroupCallConnection(
					MTP_flags(Flag::f_presentation),
					MTP_dataJSON(MTP_bytes(data.value("params").toString().toUtf8()))),
				MTP_updateGroupCallParticipants(
					InputCall(id, accessHash),
					ParseParticipants(data.value("participants").toArray()),
					MTP_int(data.value("version").toInt())),
			}));
		});
	});
}

void LeavePresentation(const Context &context, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	const auto target = ReadTarget(call);
	if (!reader.ok()) {
		context.answer.done(Updates({}));
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "DELETE", CallPath(id, u"/presentation"_q), {}, [=](
				QJsonObject data) {
			answer.done(ParticipantsUpdates(InputCall(id, accessHash), data));
		});
	});
}

void GetChainBlocks(const Context &context, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	const auto target = ReadTarget(call);
	const auto subChain = reader.read<MTPint>().v;
	const auto offset = reader.read<MTPint>().v;
	const auto limit = reader.read<MTPint>().v;
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		const auto path = CallPath(
			id,
			u"/chain/%1?offset=%2&limit=%3"_q.arg(
				subChain).arg(offset).arg(limit));
		Request(context, "GET", path, {}, [=](QJsonObject data) {
			answer.done(Updates({ MTP_updateGroupCallChainBlocks(
				InputCall(id, accessHash),
				MTP_int(data.value("sub_chain_id").toInt()),
				ParseBlocks(data.value("blocks").toArray()),
				MTP_int(data.value("next_offset").toInt())) }));
		});
	});
}

void SendBroadcast(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto block = reader.read<MTPbytes>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	// The block comes back as an event, to its author too: the client
	// applies only blocks the server handed out.
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/chain/1"_q), {
			{ "block", Base64(block) },
		}, [=](QJsonObject) {
			answer.done(Updates({}));
		});
	});
}

void InviteParticipant(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto user = reader.read<MTPInputUser>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	// The invitation message reaches this chat as message.created, like any
	// message: answering it here as well would add it twice.
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/invite"_q), {
			{ "user_id", ReadUserId(user, context.me) },
			{ "video", (flags & (1 << 0)) != 0 },
		}, [=](QJsonObject) {
			answer.done(Updates({}));
		});
	});
}

void DeclineInvite(const Context &context, Reader &reader) {
	const auto messageId = reader.read<MTPint>();
	if (!reader.ok()) {
		context.answer.done(Updates({}));
		return;
	}
	const auto answer = context.answer;
	Request(
		context,
		"POST",
		u"/conferences/invites/%1/decline"_q.arg(messageId.v),
		{},
		[=](QJsonObject) { answer.done(Updates({})); });
}

void DeleteParticipants(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto ids = reader.read<MTPVector<MTPlong>>();
	const auto block = reader.read<MTPbytes>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	auto list = QJsonArray();
	for (const auto &id : ids.v) {
		list.push_back(qint64(id.v));
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/participants/delete"_q), {
			{ "ids", list },
			{ "only_left", (flags & (1 << 0)) != 0 },
			{ "kick", (flags & (1 << 1)) != 0 },
			{ "block", Base64(block) },
		}, [=](QJsonObject) {
			answer.done(Updates({}));
		});
	});
}

void SendEncryptedMessage(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto data = reader.read<MTPbytes>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64 accessHash) {
		Request(context, "POST", CallPath(id, u"/messages"_q), {
			{ "data", Base64(data) },
		}, [=](QJsonObject) {
			answer.done(MTPBool(MTP_boolTrue()));
		});
	});
}

// --- the video chat of a group or channel --------------------------------

[[nodiscard]] qint64 ChatIdOf(const MTPInputPeer &peer) {
	return peer.match([](const MTPDinputPeerChannel &data) {
		return qint64(data.vchannel_id().v);
	}, [](const auto &) {
		return qint64(0);
	});
}

// Joining as the group or channel itself is not offered: every member joins
// as themselves.
void GetJoinAs(const Context &context, Reader &reader) {
	[[maybe_unused]] const auto peer = reader.read<MTPInputPeer>();
	context.answer.done(MTP_phone_joinAsPeers(
		MTP_vector<MTPPeer>(QVector<MTPPeer>{
			MTP_peerUser(MTP_long(context.me)),
		}),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>()));
}

void SaveDefaultJoinAs(const Context &context, Reader &) {
	context.answer.done(MTPBool(MTP_boolTrue()));
}

void CreateChatCall(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto chatId = ChatIdOf(reader.read<MTPInputPeer>());
	[[maybe_unused]] const auto randomId = reader.read<MTPint>();
	const auto title = (flags & (1 << 0))
		? qs(reader.read<MTPstring>())
		: QString();
	const auto scheduleDate = (flags & (1 << 1))
		? reader.read<MTPint>().v
		: 0;
	if (!reader.ok() || chatId <= 0) {
		context.answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	const auto answer = context.answer;
	Request(context, "POST", u"/chats/%1/call"_q.arg(chatId), {
		{ "title", title },
		{ "schedule_date", scheduleDate },
	}, [=](QJsonObject data) {
		answer.done(Updates({ CallUpdate(data.value("call").toObject()) }));
	});
}

// A request naming a call and answering with its new state.
void CallMutation(
		const Context &context,
		const Target &target,
		const QByteArray &method,
		const QString &tail,
		const QJsonObject &body) {
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64) {
		Request(context, method, CallPath(id, tail), body, [=](
				QJsonObject data) {
			answer.done(Updates({ CallUpdate(data.value("call").toObject()) }));
		});
	});
}

void StartScheduledCall(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	CallMutation(context, target, "POST", u"/start"_q, {});
}

void ToggleStartSubscription(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto subscribed = mtpIsTrue(reader.read<MTPBool>());
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	CallMutation(
		context,
		target,
		subscribed ? "POST" : "DELETE",
		u"/subscription"_q,
		{});
}

void EditCallTitle(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto title = qs(reader.read<MTPstring>());
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	CallMutation(context, target, "PATCH", QString(), {
		{ "title", title },
	});
}

// "Share" of a group's or channel's video chat. Upstream's links, built from
// the chat: a public chat's opens the call through contacts.resolveUsername
// (?videochat, ?livestream in a channel), a private one's opens the chat,
// where members see the call. There is no speaker link - who may speak is
// the chat's rule, not the link's - and upstream shares the one link alone
// when that request fails.
void ExportCallInvite(const Context &context, Reader &reader) {
	const auto flags = reader.flags();
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto session = context.session;
	const auto call = (reader.ok() && session && target.id)
		? session->data().groupCall(target.id)
		: nullptr;
	const auto channel = call ? call->peer()->asChannel() : nullptr;
	if (!channel) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	} else if (flags & (1 << 0)) { // can_self_unmute
		context.answer.fail(u"CHAT_ADMIN_REQUIRED"_q, 403);
		return;
	}
	const auto username = channel->username();
	const auto link = !username.isEmpty()
		? session->createInternalLinkFull(username
			+ (channel->isBroadcast() ? u"?livestream"_q : u"?videochat"_q))
		: session->createInternalLinkFull(
			u"c/%1"_q.arg(peerToChannel(channel->id).bare));
	context.answer.done(MTP_phone_exportedGroupCallInvite(MTP_string(link)));
}

void InviteToChatCall(const Context &context, Reader &reader) {
	const auto target = ReadTarget(reader.read<MTPInputGroupCall>());
	const auto users = reader.read<MTPVector<MTPInputUser>>();
	if (!reader.ok()) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	auto ids = QJsonArray();
	for (const auto &user : users.v) {
		if (const auto id = ReadUserId(user, context.me)) {
			ids.push_back(id);
		}
	}
	const auto answer = context.answer;
	Resolve(context, target, [=](qint64 id, qint64) {
		Request(context, "POST", CallPath(id, u"/chat-invite"_q), {
			{ "user_ids", ids },
		}, [=](QJsonObject) {
			answer.done(Updates({}));
		});
	});
}

using Handler = void(*)(const Context &, Reader &);

[[nodiscard]] Handler HandlerFor(mtpTypeId type) {
	switch (type) {
	case mtpc_phone_createConferenceCall: return CreateConference;
	case mtpc_phone_joinGroupCall: return JoinConference;
	case mtpc_phone_leaveGroupCall: return LeaveConference;
	case mtpc_phone_discardGroupCall: return DiscardConference;
	case mtpc_phone_getGroupCall: return GetConference;
	case mtpc_phone_getGroupParticipants: return GetParticipants;
	case mtpc_phone_checkGroupCall: return CheckConference;
	case mtpc_phone_editGroupCallParticipant: return EditParticipant;
	case mtpc_phone_toggleGroupCallSettings: return ToggleSettings;
	case mtpc_phone_joinGroupCallPresentation: return JoinPresentation;
	case mtpc_phone_leaveGroupCallPresentation: return LeavePresentation;
	case mtpc_phone_getGroupCallChainBlocks: return GetChainBlocks;
	case mtpc_phone_sendConferenceCallBroadcast: return SendBroadcast;
	case mtpc_phone_inviteConferenceCallParticipant: return InviteParticipant;
	case mtpc_phone_declineConferenceCallInvite: return DeclineInvite;
	case mtpc_phone_deleteConferenceCallParticipants: return DeleteParticipants;
	case mtpc_phone_sendGroupCallEncryptedMessage: return SendEncryptedMessage;
	case mtpc_phone_getGroupCallJoinAs: return GetJoinAs;
	case mtpc_phone_exportGroupCallInvite: return ExportCallInvite;
	case mtpc_phone_saveDefaultGroupCallJoinAs: return SaveDefaultJoinAs;
	case mtpc_phone_createGroupCall: return CreateChatCall;
	case mtpc_phone_startScheduledGroupCall: return StartScheduledCall;
	case mtpc_phone_toggleGroupCallStartSubscription: return ToggleStartSubscription;
	case mtpc_phone_editGroupCallTitle: return EditCallTitle;
	case mtpc_phone_inviteToGroupCall: return InviteToChatCall;
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
		.session = session,
		.client = session ? &ClientFor(session) : nullptr,
		.answer = Answer(instance, requestId),
	};
	if (!handler || !context.client) {
		context.answer.fail(u"GROUPCALL_INVALID"_q);
		return;
	}
	context.me = context.client->meId();
	auto reader = Reader(request);
	handler(context, reader);
}

bool HandleEvent(
		Main::Session *session,
		const QString &type,
		const QJsonObject &data) {
	if (!session || !type.startsWith(u"conference."_q)) {
		return false;
	}
	EnsureUsers(session, data);
	const auto input = InputCall(
		Number(data, "call_id"),
		Number(data, "access_hash"));
	auto list = QVector<MTPUpdate>();
	if (type == u"conference.participants"_q) {
		list.push_back(MTP_updateGroupCallParticipants(
			input,
			ParseParticipants(data.value("participants").toArray()),
			MTP_int(data.value("version").toInt())));
	} else if (type == u"conference.updated"_q) {
		list.push_back(CallUpdate(data.value("call").toObject()));
	} else if (type == u"conference.chain_blocks"_q) {
		list.push_back(MTP_updateGroupCallChainBlocks(
			input,
			MTP_int(data.value("sub_chain_id").toInt()),
			ParseBlocks(data.value("blocks").toArray()),
			MTP_int(data.value("next_offset").toInt())));
	} else if (type == u"conference.encrypted_message"_q) {
		list.push_back(MTP_updateGroupCallEncryptedMessage(
			input,
			MTP_peerUser(MTP_long(Number(data, "from_id"))),
			MTP_bytes(Base64(data.value("data")))));
	} else {
		LOG(("FoxMes Conference: unknown event %1").arg(type));
		return true;
	}
	session->api().applyUpdates(Updates(std::move(list)));
	return true;
}

} // namespace CustomBackend::Conferences
