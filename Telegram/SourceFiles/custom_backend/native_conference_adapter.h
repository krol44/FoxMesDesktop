/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

class QJsonObject;
class QString;

namespace Main {
class Session;
}

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
}
}

namespace CustomBackend::Conferences {

[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

[[nodiscard]] bool HandleEvent(
	Main::Session *session,
	const QString &type,
	const QJsonObject &data);

}
