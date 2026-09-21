/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

class QJsonObject;
class QString;

namespace Main {
class Session;
} // namespace Main

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP

namespace CustomBackend::Conferences {

// Group calls are answered at the MTProto boundary, not at the call sites.
//
// Upstream's group call (GroupCall, Data::GroupCall, the invite box, the
// panel) makes about twenty different phone.* requests from a dozen places.
// Instead of a hook at each, MTP::Instance hands every request whose
// constructor is listed here to this adapter, which reads the TL request,
// asks fxl-api, builds the TL answer upstream expects and delivers it through
// the regular response path (Instance::processCallback). To upstream the
// server simply answered.
//
// Requests not listed here keep today's behaviour: under the bridge they are
// never sent anywhere.
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

// conference.* WebSocket events, turned back into the updates upstream
// expects and applied like any other server updates. Returns false for an
// event this adapter does not own.
[[nodiscard]] bool HandleEvent(
	Main::Session *session,
	const QString &type,
	const QJsonObject &data);

} // namespace CustomBackend::Conferences
