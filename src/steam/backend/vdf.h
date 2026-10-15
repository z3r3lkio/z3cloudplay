#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace z3cloudplay::steam {

inline constexpr std::size_t kMaxVdfBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxVdfDepth = 64;
inline constexpr std::size_t kMaxVdfNodes = 1'000'000;

// Valve text KeyValues node: either a string value or an ordered list of children.
struct VdfNode {
  bool is_dict = false;
  std::string value;
  std::vector<std::pair<std::string, VdfNode>> children;

  // ASCII case-insensitive lookup of the first matching child. Null for string nodes.
  const VdfNode* Find(std::string_view key) const;
  // String value of a child; nullopt when missing or when the child is a dict.
  std::optional<std::string_view> String(std::string_view key) const;
  // Child string value parsed fully as an unsigned decimal u64.
  std::optional<std::uint64_t> Uint64(std::string_view key) const;
};

// Parses text VDF. The root is a dict of the top-level pairs. Callers strip trailing NULs;
// any NUL left in the text is an error, as are malformed input and exceeding the size, depth
// or node caps.
std::optional<VdfNode> ParseVdf(std::string_view text);

}  // namespace z3cloudplay::steam
