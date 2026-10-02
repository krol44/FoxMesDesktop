#pragma once

#include "data/data_types.h"

#include <functional>

class History;

namespace CustomBackend {


void ReadHistory(
	not_null<History*> history,
	MsgId tillId,
	std::function<void()> done);

void SetChatUnreadMark(not_null<History*> history, bool unread);

[[nodiscard]] bool ShouldBeInChatList(not_null<const History*> history);

}
