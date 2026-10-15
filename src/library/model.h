#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace z3cloudplay {
using GameId = std::uint32_t; // Steam app ID, stable across refreshes and account sessions.

struct GameSummary {
    GameId id = 0;
    std::string title;
    std::string artwork_source;
    std::string description;
    std::string developer;
    std::optional<std::uint64_t> playtime_minutes;
    std::optional<std::uint64_t> last_played_unix_seconds;
    // Earliest license acquisition, from the Steam CM; absent when CM was unavailable.
    std::optional<std::uint64_t> added_at_unix_seconds;
};
} // namespace z3cloudplay
