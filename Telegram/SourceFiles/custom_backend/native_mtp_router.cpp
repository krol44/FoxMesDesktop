/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_mtp_router.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "custom_backend/native_channels_adapter.h"
#include "custom_backend/native_conference_adapter.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QJsonObject>

namespace CustomBackend::Mtp {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;

constexpr auto kBody = SerializedRequest::kMessageBodyPosition;

} // namespace

mtpTypeId RequestType(const SerializedRequest &request) {
	if (!request || request->size() <= kBody) {
		return 0;
	}
	return mtpTypeId((*request)[kBody]);
}

Main::Session *SessionFor(not_null<MTP::Instance*> instance) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (&account->mtp() == instance.get() && account->sessionExists()) {
			return &account->session();
		}
	}
	return nullptr;
}

Reader::Reader(const SerializedRequest &request)
// Past the constructor: the readers take the fields in TL order.
: _from(request->constData() + kBody + 1)
, _end(request->constData() + request->size()) {
}

Answer::Answer(not_null<MTP::Instance*> instance, mtpRequestId requestId)
: _instance(instance.get())
, _requestId(requestId) {
}

void Answer::fail(const QString &type, int code) const {
	auto reply = mtpBuffer();
	MTPRpcError(MTP_rpc_error(MTP_int(code), MTP_string(type))).write(reply);
	deliver(std::move(reply));
}

// Always on a later turn of the event loop, as a network answer would come:
// upstream stores the id send() returns (ChooseJoinAsProcess::requestList
// writes it into state its done() handler destroys), so an answer delivered
// from inside send() - a handler that needs no server - ran done() first and
// crashed on the write that followed.
void Answer::deliver(mtpBuffer &&reply) const {
	crl::on_main([
		instance = _instance,
		requestId = _requestId,
		reply = std::move(reply)
	]() mutable {
		if (const auto strong = instance.data()) {
			strong->processCallback(MTP::Response{
				.reply = std::move(reply),
				.outerMsgId = mtpMsgId(base::unixtime::mtproto_msg_id()),
				.requestId = requestId,
			});
		}
	});
}

QString ErrorType(
		const QJsonDocument &doc,
		const QString &error,
		int status,
		const QString &fallback404,
		const QString &fallback403) {
	const auto code = doc.isObject()
		? doc.object().value("code").toString()
		: QString();
	if (!code.isEmpty() && code.toUpper() == code) {
		return code;
	} else if (status == 404) {
		return fallback404;
	} else if (status == 403) {
		return fallback403;
	}
	return error.isEmpty() ? u"INTERNAL_SERVER_ERROR"_q : error;
}

bool Intercepts(const SerializedRequest &request) {
	return Conferences::Intercepts(request) || Channels::Intercepts(request);
}

void Intercept(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const SerializedRequest &request) {
	if (Conferences::Intercepts(request)) {
		Conferences::Intercept(instance, requestId, request);
	} else {
		Channels::Intercept(instance, requestId, request);
	}
}

} // namespace CustomBackend::Mtp
