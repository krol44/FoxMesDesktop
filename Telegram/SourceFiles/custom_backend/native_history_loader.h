#pragma once

#include "data/data_messages.h"

class History;

namespace MTP {
class Error;
}

namespace Main {
class Session;
}

namespace Ui {
class ElasticScroll;
}

namespace Window {
class SessionController;
}

namespace CustomBackend {

struct HistoryLoadingView {
    not_null<Window::SessionController*> controller;
    not_null<Ui::ElasticScroll*> area;
};
[[nodiscard]] bool WidgetHistoryRequest(
    not_null<Main::Session*> session,
    not_null<History*> history,
    MsgId showAtMsgId,
    Data::LoadDirection direction,
    Fn<void()> done,
    Fn<void(const MTP::Error &)> failed,
    std::optional<HistoryLoadingView> loading = std::nullopt);

}
