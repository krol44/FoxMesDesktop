#include "custom_backend/native_chat_state_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "history/history.h"
#include "data/data_peer.h"
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
        return;
    }
    bridge->setChatUnreadMark(history, unread);
}

bool ShouldBeInChatList(not_null<const History*> history) {
    const auto bridge = BridgeFor(&history->session());
    return bridge
        && history->folderKnown()
        && !history->peer->migrateTo()
        && bridge->hasChat(history->peer->id);
}

}
