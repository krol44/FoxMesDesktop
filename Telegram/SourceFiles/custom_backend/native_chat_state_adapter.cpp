#include "custom_backend/native_chat_state_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "history/history.h"
#include "main/main_session.h"

namespace CustomBackend {

void ReadHistory(
        not_null<History*> history,
        MsgId tillId,
        std::function<void()> done) {
    const auto bridge = Enabled()
        ? BridgeFor(&history->session())
        : nullptr;
    if (!bridge) {
        // The backend owns read state whenever it is enabled, so falling back
        // to MTProto here would send a request FoxMes never accounts for.
        // Report completion instead: upstream releases its request slot and
        // the read is retried by the next sendReadRequest.
        if (done) done();
        return;
    }
    bridge->readHistory(history, tillId.bare, std::move(done));
}

void SetChatUnreadMark(not_null<History*> history, bool unread) {
    const auto bridge = Enabled()
        ? BridgeFor(&history->session())
        : nullptr;
    if (!bridge) {
        // Local flag is already set by the caller; without the bridge there is
        // nothing to persist and MTProto must not be used.
        return;
    }
    bridge->setChatUnreadMark(history, unread);
}

} // namespace CustomBackend
