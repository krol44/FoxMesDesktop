#pragma once

#include "mtproto/core_types.h"
#include "data/data_types.h"

#include <functional>
#include <vector>

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
}

namespace CustomBackend {

[[nodiscard]] bool NativeDeleteEffectAvailable();
void DeleteMessagesWithEffect(
    Main::Session *session,
    const std::vector<not_null<HistoryItem*>> &items);
void DeleteSelectedMessages(
    not_null<Main::Session*> session,
    const MessageIdsList &ids,
    bool revoke);

void DeleteMessages(
    not_null<History*> history,
    const QVector<MTPint> &ids,
    bool revoke,
    std::function<void()> finish);
void DeleteHistory(not_null<PeerData*> peer, bool justClear, bool revoke);
void DeleteMessagesByDates(
    not_null<History*> history,
    TimeId minDate,
    TimeId maxDate);

}
