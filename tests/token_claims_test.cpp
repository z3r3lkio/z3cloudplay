#include "steam/backend/token_claims.h"
#include <cassert>
#include <cstdint>
#include <string>

namespace {
std::string Base64Url(const std::string& input) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  for (std::size_t at = 0; at < input.size(); at += 3) {
    const std::uint32_t a = static_cast<unsigned char>(input[at]);
    const std::uint32_t b = at + 1 < input.size() ? static_cast<unsigned char>(input[at + 1]) : 0;
    const std::uint32_t c = at + 2 < input.size() ? static_cast<unsigned char>(input[at + 2]) : 0;
    const std::uint32_t bits = (a << 16) | (b << 8) | c;
    output.push_back(alphabet[(bits >> 18) & 63]); output.push_back(alphabet[(bits >> 12) & 63]);
    if (at + 1 < input.size()) output.push_back(alphabet[(bits >> 6) & 63]);
    if (at + 2 < input.size()) output.push_back(alphabet[bits & 63]);
  }
  return output;
}
std::string Token(const std::string& payload) { return Base64Url(R"({"alg":"none"})") + "." + Base64Url(payload) + ".AA"; }
}

int main() {
  auto claims = z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"76561198012345678","exp":4102444800})"));
  assert(claims && claims->steam_id == 76561198012345678ULL && claims->expires_at_unix_seconds == 4102444800ULL);
  claims = z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"18446744073709551615","exp":18446744073709551615})"));
  assert(claims && claims->steam_id == UINT64_MAX && claims->expires_at_unix_seconds == UINT64_MAX);

  assert(!z3cloudplay::steam::InspectTokenClaims("AA.AA"));
  assert(!z3cloudplay::steam::InspectTokenClaims("AA.AA.AA.AA"));
  assert(!z3cloudplay::steam::InspectTokenClaims("A.AA.AA"));
  assert(!z3cloudplay::steam::InspectTokenClaims("AB.AA.AA"));
  assert(!z3cloudplay::steam::InspectTokenClaims("AA.AA.A="));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"1","sub":"2","exp":3})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"1","exp":3,"exp":4})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":1,"exp":3})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"1","exp":"3"})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"0","exp":3})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"1","exp":0})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token(R"({"sub":"18446744073709551616","exp":3})")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token("{\"sub\":\"1\",\"exp\":3,\"x\":\"\xc0\x80\"}")));
  assert(!z3cloudplay::steam::InspectTokenClaims(Token("[[[[[[[[[[0]]]]]]]]]]")));
  assert(!z3cloudplay::steam::InspectTokenClaims(std::string(24 * 1024 + 1, 'A')));
  return 0;
}
