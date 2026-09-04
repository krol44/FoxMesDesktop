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
} // namespace Main

namespace CustomBackend {

[[nodiscard]] bool NativeDeleteEffectAvailable();
void DeleteMessagesWithEffect(
    Main::Session *session,
    const std::vector<not_null<HistoryItem*>> &items);
void DeleteSelectedMessages(
    not_null<Main::Session*> session,
    const MessageIdsList &ids,
    bool revoke);

// Transport replacement for Histories::deleteMessages: sends the batch
// delete through the FoxMes REST API and invokes `finish` on any HTTP
// outcome (success or error), preserving the upstream RequestType::Delete
// state machine of Histories::sendRequest.
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

} // namespace CustomBackend
