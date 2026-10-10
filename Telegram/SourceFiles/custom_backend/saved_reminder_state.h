#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace CustomBackend {

struct SavedReminderMessage {
    std::int64_t id = 0;
    bool scheduled = false;
};

inline std::optional<int> CountSavedReminders(
        const std::vector<SavedReminderMessage> &messages,
        std::int64_t readBefore,
        std::int64_t readTill,
        std::int64_t firstLoaded,
        std::int64_t lastLoaded,
        std::optional<int> unreadCount,
        bool loadedAtTop,
        bool loadedAtBottom) {
    if (unreadCount && firstLoaded <= readBefore && lastLoaded >= readTill) {
        const auto read = std::count_if(messages.begin(), messages.end(), [&](const auto &message) {
            return message.scheduled && message.id >= readBefore && message.id <= readTill;
        });
        return std::max(*unreadCount - int(read), 0);
    }
    if (!loadedAtBottom || (!loadedAtTop && !firstLoaded) || firstLoaded > readTill) {
        return std::nullopt;
    }
    return int(std::count_if(messages.begin(), messages.end(), [&](const auto &message) {
        return message.scheduled && message.id > readTill;
    }));
}

}
