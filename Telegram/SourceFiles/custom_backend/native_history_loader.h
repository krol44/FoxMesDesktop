#pragma once

#include "mtproto/core_types.h"

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
}
}

namespace Ui {
class ElasticScroll;
}

namespace Window {
class SessionController;
}

namespace CustomBackend {

void ShowHistoryLoading(
    not_null<Window::SessionController*> controller,
    not_null<Ui::ElasticScroll*> area,
    Fn<bool()> waiting);

}

namespace CustomBackend::HistoryPages {

[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
    not_null<MTP::Instance*> instance,
    mtpRequestId requestId,
    const MTP::details::SerializedRequest &request);

}
