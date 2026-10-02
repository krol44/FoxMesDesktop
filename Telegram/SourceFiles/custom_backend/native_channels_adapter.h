/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
}
}

namespace CustomBackend::Channels {

[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

}
