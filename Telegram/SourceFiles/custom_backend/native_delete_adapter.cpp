#include "custom_backend/native_delete_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "base/flat_map.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/effects/thanos_effect.h"

namespace CustomBackend {

bool NativeDeleteEffectAvailable() {
    return Ui::ThanosEffect::Supported();
}

void DeleteMessagesWithEffect(
        Main::Session *session,
        const std::vector<not_null<HistoryItem*>> &items) {
    if (!session || items.empty()) {
        return;
    }
    session->data().notifyItemsAboutToBeDestroyed(items);
    for (const auto &item : items) {
        item->destroy();
    }
}

void DeleteSelectedMessages(
        not_null<Main::Session*> session,
        const MessageIdsList &ids,
        bool revoke) {
    const auto bridge = Enabled() ? BridgeFor(session) : nullptr;
    if (!bridge) {
        return;
    }
    auto grouped = base::flat_map<not_null<History*>, std::vector<int32_t>>();
    // Local items - a send that is still uploading, or one that failed - have
    // no server id, so only the client can remove them. Upstream destroys them
    // together with the server-side ones; skipping them here is what made a
    // stuck message impossible to delete or cancel.
    auto local = std::vector<not_null<HistoryItem*>>();
    for (const auto &fullId : ids) {
        const auto item = session->data().message(fullId);
        if (!item) {
            continue;
        }
        if (!item->isRegular() || item->id.bare <= 0) {
            local.push_back(item);
            continue;
        }
        grouped[item->history()].push_back(item->id.bare);
    }
    for (auto &[history, messageIds] : grouped) {
        bridge->deleteMessages(history, messageIds, revoke);
    }
    if (!local.empty()) {
        // Destroying a sending item runs upstream's cancelLocalItem, which is
        // the hook that aborts the transfer behind it.
        DeleteMessagesWithEffect(session, local);
        session->data().sendHistoryChangeNotifications();
    }
}

void DeleteMessages(
        not_null<History*> history,
        const QVector<MTPint> &ids,
        bool revoke,
        std::function<void()> finish) {
    const auto bridge = Enabled()
        ? BridgeFor(&history->session())
        : nullptr;
    if (!bridge) {
        if (finish) finish();
        return;
    }
    auto plain = std::vector<int32_t>();
    plain.reserve(ids.size());
    for (const auto &id : ids) plain.push_back(id.v);
    bridge->deleteMessages(history, std::move(plain), revoke, std::move(finish));
}

void DeleteHistory(not_null<PeerData*> peer, bool justClear, bool) {
    const auto bridge = Enabled()
        ? BridgeFor(&peer->session())
        : nullptr;
    if (!bridge) {
        return;
    }
    bridge->deleteHistory(
        peer->owner().history(peer),
        !justClear && !peer->isSelf());
}

void DeleteMessagesByDates(
        not_null<History*> history,
        TimeId minDate,
        TimeId maxDate) {
    const auto bridge = Enabled()
        ? BridgeFor(&history->session())
        : nullptr;
    if (!bridge) {
        return;
    }
    bridge->deleteMessagesByDates(history, minDate, maxDate);
}

} // namespace CustomBackend
