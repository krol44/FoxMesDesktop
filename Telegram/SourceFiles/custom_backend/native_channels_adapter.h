/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP

namespace CustomBackend::Channels {

// Groups and channels at the MTProto boundary (see native_mtp_router.h):
// channels.*, contacts.resolveUsername and the ephemeral.* welcome messages,
// answered from fxl-api's /chats routes. Requests for features FoxMes keeps
// hidden (sponsored messages, send-as, recommendations) get the empty answer
// upstream already handles, so none of them holds its request slot forever.
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

} // namespace CustomBackend::Channels
