#pragma once

#include "data/data_types.h"

#include <functional>
#include <optional>

class HistoryItem;
namespace HistoryView { class Element; }

class History;

namespace CustomBackend {


void ReadHistory(
	not_null<History*> history,
	MsgId tillId,
	std::function<void()> done);

void SetChatUnreadMark(not_null<History*> history, bool unread);

[[nodiscard]] std::optional<int> CountUnreadSavedReminders(
    not_null<const History*> history, MsgId readTillId);
[[nodiscard]] HistoryView::Element *FirstUnreadSavedReminder(not_null<History*> history);
[[nodiscard]] bool IsUnreadSavedReminder(not_null<const HistoryItem*> item);

[[nodiscard]] bool ShouldBeInChatList(not_null<const History*> history);

}
