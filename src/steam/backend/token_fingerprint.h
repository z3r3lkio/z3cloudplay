#pragma once

#include "steam/backend/token_claims.h"

#include <mbedtls/sha256.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace z3cloudplay::steam {

// For logs: "fp=<first 8 hex of SHA-256 of the token> exp=<expiry, unix seconds>". The token itself
// can't be recovered from it, but two log lines show whether they used the same token.
inline std::string TokenFingerprint(std::string_view token) {
  unsigned char digest[32] = {};
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest, 0);
  const auto claims = InspectTokenClaims(token);
  char text[64];
  std::snprintf(text, sizeof(text), "fp=%02x%02x%02x%02x exp=%llu", digest[0], digest[1], digest[2], digest[3],
                static_cast<unsigned long long>(claims ? claims->expires_at_unix_seconds : 0));
  return text;
}

}  // namespace z3cloudplay::steam
