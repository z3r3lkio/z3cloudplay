#include "steam/backend/json.h"
#include <cassert>
#include <cstdint>
#include <string>

using namespace z3cloudplay::steam::json;

int main() {
  auto parsed = Parse(R"({"id":18446744073709551615,"escaped":"A\u00df\ud83d\ude80","items":[true,null,-1.25e+3]})");
  assert(parsed && parsed->is_object());
  const Value* id = parsed->Find("id");
  assert(id && id->text() == "18446744073709551615" && id->AsUint64() == UINT64_MAX);
  assert(parsed->Find("escaped")->text() == std::string("A\xc3\x9f\xf0\x9f\x9a\x80"));
  assert(parsed->Find("items")->array().size() == 3);

  assert(!Parse(R"({"a":1,"a":2})"));
  assert(!Parse(R"({"a":1,"\u0061":2})"));
  assert(!Parse("[1] trailing"));
  assert(!Parse("01")); assert(!Parse("1.")); assert(!Parse("+1"));
  assert(!Parse("NaN")); assert(!Parse("Infinity"));
  assert(!Parse("\"\\ud800\"")); assert(!Parse("\"\\udc00\""));
  assert(!Parse(std::string("\"\xc0\x80\"", 4)));
  assert(!Parse(std::string("\"\xed\xa0\x80\"", 5)));
  assert(!Parse(std::string("\"a\x01" "b\"", 5)));

  Limits limits; limits.max_depth = 2;
  assert(Parse("[[0]]", limits)); assert(!Parse("[[[0]]]", limits));
  limits = {}; limits.max_values = 3;
  assert(Parse("[1,2]", limits)); assert(!Parse("[1,2,3]", limits));
  limits = {}; limits.max_string_bytes = 3;
  assert(Parse("\"abc\"", limits)); assert(!Parse("\"abcd\"", limits));
  limits = {}; limits.max_bytes = 2;
  assert(!Parse("null", limits));

  auto integer = Parse("0"); assert(integer && integer->AsUint64() == 0);
  integer = Parse("-1"); assert(integer && !integer->AsUint64());
  integer = Parse("1.0"); assert(integer && !integer->AsUint64());
  integer = Parse("18446744073709551616"); assert(integer && !integer->AsUint64());
  return 0;
}
