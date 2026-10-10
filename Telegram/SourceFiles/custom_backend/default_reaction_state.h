#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CustomBackend::Reactions {

// Transport-independent account state. Asset readiness never participates in ID selection.
struct DefaultReactionState final {
    using Id = std::uint64_t;
    struct Entry { Id id; std::string emoji; };
    struct Mutation { Id id; std::int64_t revision; bool unchanged; };
    Id confirmed = 0;
    std::int64_t revision = -1;
    std::optional<Id> desired;
    std::optional<Id> sent;

    bool receive(Id id, std::int64_t nextRevision) {
        if (nextRevision < 0 || nextRevision < revision) return false;
        confirmed = id;
        revision = nextRevision;
        return true;
    }

    std::optional<Mutation> begin() {
        if (sent || !desired || revision < 0) return std::nullopt;
        const auto id = *desired;
        desired.reset();
        sent = id;
        return Mutation{ id, revision, id == confirmed };
    }

    void finish(bool success) {
        sent.reset();
        if (!success) desired.reset();
    }

    Id effective(const std::vector<Entry> &catalog) const {
        const auto chosen = desired.value_or(sent.value_or(confirmed));
        for (const auto &entry : catalog) if (entry.id == chosen) return chosen;
        for (const auto &entry : catalog) if (entry.emoji == "👍" || entry.emoji == "👍️") return entry.id;
        return catalog.empty() ? 0 : catalog.front().id;
    }
};

} // namespace CustomBackend::Reactions
