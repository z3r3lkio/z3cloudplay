#include "steam/backend/token_claims.h"

#include "steam/backend/json.h"

#include <string>

namespace z3cloudplay::steam {
namespace {
constexpr std::size_t kMaximumTokenBytes = 24 * 1024;
constexpr std::size_t kMaximumPayloadBytes = 18 * 1024;

int DecodeCharacter(char byte) {
  if (byte >= 'A' && byte <= 'Z') return byte - 'A';
  if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
  if (byte >= '0' && byte <= '9') return byte - '0' + 52;
  if (byte == '-') return 62;
  if (byte == '_') return 63;
  return -1;
}

bool ValidBase64Url(std::string_view input) {
  if (input.empty() || input.size() % 4 == 1) return false;
  for (char byte : input) if (DecodeCharacter(byte) < 0) return false;
  if (input.size() % 4 == 2 && (DecodeCharacter(input.back()) & 0x0f) != 0) return false;
  if (input.size() % 4 == 3 && (DecodeCharacter(input.back()) & 0x03) != 0) return false;
  return true;
}

bool DecodeBase64Url(std::string_view input, std::string& output) {
  if (!ValidBase64Url(input)) return false;
  const std::size_t decoded_size = (input.size() / 4) * 3 + (input.size() % 4 == 2 ? 1 : input.size() % 4 == 3 ? 2 : 0);
  if (decoded_size > kMaximumPayloadBytes) return false;
  output.clear(); output.reserve(decoded_size);
  std::uint32_t accumulator = 0; unsigned bits = 0;
  for (char byte : input) {
    accumulator = (accumulator << 6) | static_cast<unsigned>(DecodeCharacter(byte)); bits += 6;
    if (bits >= 8) { bits -= 8; output.push_back(static_cast<char>((accumulator >> bits) & 0xff)); }
  }
  return output.size() == decoded_size;
}
}

std::optional<TokenClaims> InspectTokenClaims(std::string_view token) {
  if (token.empty() || token.size() > kMaximumTokenBytes) return std::nullopt;
  const std::size_t first = token.find('.');
  if (first == std::string_view::npos) return std::nullopt;
  const std::size_t second = token.find('.', first + 1);
  if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) return std::nullopt;
  const auto header = token.substr(0, first); const auto payload = token.substr(first + 1, second - first - 1); const auto signature = token.substr(second + 1);
  if (!ValidBase64Url(header) || !ValidBase64Url(signature)) return std::nullopt;
  std::string decoded;
  if (!DecodeBase64Url(payload, decoded)) return std::nullopt;
  json::Limits limits; limits.max_bytes = kMaximumPayloadBytes; limits.max_depth = 8; limits.max_values = 256; limits.max_string_bytes = 16 * 1024;
  const auto root = json::Parse(decoded, limits);
  if (!root || !root->is_object()) return std::nullopt;
  const json::Value* subject = root->Find("sub"); const json::Value* expiration = root->Find("exp");
  if (!subject || !subject->is_string() || !expiration || !expiration->is_number()) return std::nullopt;
  const std::string& subject_text = subject->text();
  if (subject_text.empty()) return std::nullopt;
  std::uint64_t steam_id = 0;
  for (unsigned char byte : subject_text) {
    if (byte < '0' || byte > '9') return std::nullopt;
    const std::uint64_t digit = byte - '0';
    if (steam_id > (UINT64_MAX - digit) / 10) return std::nullopt;
    steam_id = steam_id * 10 + digit;
  }
  const auto expires = expiration->AsUint64();
  if (steam_id == 0 || !expires || *expires == 0) return std::nullopt;
  return TokenClaims{steam_id, *expires};
}

}  // namespace z3cloudplay::steam
