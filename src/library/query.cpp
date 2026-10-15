#include "library/query.h"
#include <utf8proc.h>
#include <algorithm>
#include <cstdlib>
#include <unordered_set>

namespace z3cloudplay {
std::string FoldLibraryText(const std::string& value) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto count = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
        static_cast<utf8proc_ssize_t>(value.size()), &mapped,
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_CASEFOLD));
    if (count < 0) return value; // Malformed input has deterministic byte-exact behavior.
    std::string result(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(count));
    std::free(mapped);
    return result;
}
namespace {
struct Match { GameId id; std::string title; };
}

std::vector<GameId> QueryLibrary(const std::vector<GameSummary>& games,
                                const LibraryQuery& query, LibrarySort sort) {
    const auto text = FoldLibraryText(query.text);
    const auto developer = FoldLibraryText(query.developer);
    std::vector<Match> matches;
    matches.reserve(games.size());
    std::unordered_set<GameId> seen;
    for (const auto& game : games) {
        if (game.id == 0 || !seen.insert(game.id).second) continue;
        auto title = FoldLibraryText(game.title);
        if (!text.empty() && title.find(text) == std::string::npos) continue;
        if (!developer.empty() && FoldLibraryText(game.developer) != developer) continue;
        matches.push_back({game.id, std::move(title)});
    }
    std::sort(matches.begin(), matches.end(), [sort](const Match& a, const Match& b) {
        if (sort == LibrarySort::AppId || a.title == b.title) return a.id < b.id;
        return sort == LibrarySort::TitleDescending ? a.title > b.title : a.title < b.title;
    });
    std::vector<GameId> result;
    result.reserve(matches.size());
    for (const auto& match : matches) result.push_back(match.id);
    return result;
}
} // namespace z3cloudplay
