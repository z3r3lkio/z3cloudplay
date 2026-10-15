#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace z3cloudplay::steam {

struct TokenClaims {
  std::uint64_t steam_id = 0;
  std::uint64_t expires_at_unix_seconds = 0;
};

// Inspects local JWT claims only. This does not validate a JWT signature or establish trust.
std::optional<TokenClaims> InspectTokenClaims(std::string_view token);

}  // namespace z3cloudplay::steam
