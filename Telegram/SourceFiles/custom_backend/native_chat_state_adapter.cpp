#include "custom_backend/native_chat_state_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "data/data_history_messages.h"
#include "data/data_session.h"
#include "custom_backend/saved_reminder_state.h"
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

std::optional<int> CountUnreadSavedReminders(
        not_null<const History*> history, MsgId readTillId) {
    if (!history->folderKnown()) return std::nullopt;
    if (history->isEmpty()) {
        const auto messages = const_cast<History*>(history.get())->maybeMessages();
        if (!messages) return std::nullopt;
        return messages->countAfter(readTillId, 10000, [&](MsgId id) {
            const auto item = history->owner().message(history->peer->id, id);
            return item && item->isRegular() && item->isFromScheduled();
        });
    }
    auto messages = std::vector<SavedReminderMessage>();
    for (const auto &block : history->blocks) {
        for (const auto &view : block->messages) {
            const auto item = view->data();
            if (item->isRegular()) messages.push_back({ item->id.bare, item->isFromScheduled() });
        }
    }
    return CountSavedReminders(messages,
        history->inboxReadTillId().bare + 1, readTillId.bare,
        history->minMsgId().bare, history->maxMsgId().bare,
        history->unreadCountKnown() ? std::optional<int>(history->unreadCount()) : std::nullopt,
        history->loadedAtTop(), history->loadedAtBottom());
}

HistoryView::Element *FirstUnreadSavedReminder(not_null<History*> history) {
    for (const auto &block : history->blocks) {
        for (const auto &view : block->messages) {
            const auto item = view->data();
            if (item->isRegular() && IsUnreadSavedReminder(item)) return view.get();
        }
    }
    return nullptr;
}

bool IsUnreadSavedReminder(not_null<const HistoryItem*> item) {
    return item->isFromScheduled() && item->id > item->history()->inboxReadTillId();
}

bool ShouldBeInChatList(not_null<const History*> history) {
    const auto bridge = BridgeFor(&history->session());
    return bridge
        && history->folderKnown()
        && !history->peer->migrateTo()
        && bridge->hasChat(history->peer->id);
}

}
