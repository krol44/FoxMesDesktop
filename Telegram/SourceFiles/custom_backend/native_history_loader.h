#pragma once

#include "data/data_messages.h"

class History;

namespace Main {
class Session;
} // namespace Main

namespace CustomBackend {

// Routes a classic HistoryWidget page load through the FoxMes bridge.
//
// The anchor is derived from showAtMsgId: real message ids are used as-is,
// ShowAtUnreadMsgId resolves via history->loadAroundId(), and everything
// else (ShowAtTheEndMsgId, MaxMessagePosition) loads the newest page.
//
// The loaded page is applied to the History by NativeBridge; done is
// invoked on the main thread once the request settles (including failures
// and no-op cases), so the caller can release its in-flight markers.
//
// Returns false when the custom backend is disabled or no bridge is
// attached: the caller must fall back to the MTProto path.
[[nodiscard]] bool WidgetHistoryRequest(
    not_null<Main::Session*> session,
    not_null<History*> history,
    MsgId showAtMsgId,
    Data::LoadDirection direction,
    Fn<void()> done);

} // namespace CustomBackend
