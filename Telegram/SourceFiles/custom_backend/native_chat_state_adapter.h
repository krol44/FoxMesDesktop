#pragma once

#include "data/data_types.h"

#include <functional>

class History;

namespace CustomBackend {

// Read/unread-mark transport for the upstream Histories hooks. These entry
// points exist so the upstream files never have to ask whether the bridge is
// ready: when the backend is enabled, FoxMes owns the operation and MTProto is
// never reached, even while the bridge is still being attached.

// Marks the history read up to tillId. done() runs on any outcome, including
// "no bridge yet", so the upstream request slot is always released.
void ReadHistory(
	not_null<History*> history,
	MsgId tillId,
	std::function<void()> done);

// Per-user "marked as unread" dialog flag.
void SetChatUnreadMark(not_null<History*> history, bool unread);

} // namespace CustomBackend
