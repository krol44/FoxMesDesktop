#include "custom_backend/native_history_loader.h"

#include "logs.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"

#include "data/data_msg_id.h"
#include "history/history.h"

namespace CustomBackend {

bool WidgetHistoryRequest(
        not_null<Main::Session*> session,
        not_null<History*> history,
        MsgId showAtMsgId,
        Data::LoadDirection direction,
        Fn<void()> done) {
    if (!Enabled()) {
        return false;
    }
    const auto bridge = BridgeFor(session);
    if (!bridge) {
        return false;
    }
    auto aroundId = ShowAtTheEndMsgId.bare;
    if (showAtMsgId > 0 && showAtMsgId < ServerMaxMsgId) {
        aroundId = showAtMsgId.bare;
    } else if (showAtMsgId == ShowAtUnreadMsgId) {
        if (const auto around = history->loadAroundId()) {
            aroundId = around.bare;
        }
    }
    bridge->loadHistory(
        history,
        aroundId,
        direction,
        std::move(done));
    return true;
}

} // namespace CustomBackend
