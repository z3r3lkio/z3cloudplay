#include "steam/backend/binary_kv.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using z3cloudplay::steam::PackageContents;
using z3cloudplay::steam::ParsePackageBuffer;
using Buffer = std::vector<std::uint8_t>;

void Str(Buffer* b, const std::string& s) {
  b->insert(b->end(), s.begin(), s.end());
  b->push_back(0);
}

void Le(Buffer* b, std::uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void Begin(Buffer* b, const std::string& key) {
  b->push_back(0);
  Str(b, key);
}
void End(Buffer* b) { b->push_back(8); }
void Int32(Buffer* b, const std::string& key, std::uint32_t v) {
  b->push_back(2);
  Str(b, key);
  Le(b, v, 4);
}
void Int64(Buffer* b, const std::string& key, std::uint64_t v) {
  b->push_back(7);
  Str(b, key);
  Le(b, v, 8);
}
void Float(Buffer* b, const std::string& key, std::uint32_t bits) {
  b->push_back(3);
  Str(b, key);
  Le(b, bits, 4);
}
void String(Buffer* b, const std::string& key, const std::string& v) {
  b->push_back(1);
  Str(b, key);
  Str(b, v);
}

Buffer Header() { return Buffer{1, 0, 0, 0}; }

// Wraps `body` (entries of the package section) as {"<id>": {body}}.
Buffer Package(const std::string& id, const Buffer& body) {
  Buffer b = Header();
  Begin(&b, id);
  b.insert(b.end(), body.begin(), body.end());
  End(&b);
  End(&b);
  return b;
}

Buffer AppIdsBody(const Buffer& entries) {
  Buffer body;
  Begin(&body, "appids");
  body.insert(body.end(), entries.begin(), entries.end());
  End(&body);
  return body;
}

bool Parse(const Buffer& b, std::uint32_t id, PackageContents* out) {
  return ParsePackageBuffer(b.data(), b.size(), id, out);
}

void TestNormal() {
  Buffer body;
  Begin(&body, "appids");
  Int32(&body, "0", 20);
  Int32(&body, "1", 10);
  String(&body, "2", "30");
  Int32(&body, "3", 10);  // duplicate id value is de-duplicated
  End(&body);
  Begin(&body, "depotids");
  Int32(&body, "0", 11);
  End(&body);
  Int32(&body, "billingtype", 1);
  Float(&body, "ratio", 0x3f800000);
  Begin(&body, "extended");
  String(&body, "x", "y");
  Begin(&body, "inner");
  Int64(&body, "big", 0x1122334455667788ULL);
  End(&body);
  End(&body);
  PackageContents out;
  assert(Parse(Package("1234", body), 1234, &out));
  assert((out.app_ids == std::vector<std::uint32_t>{10, 20, 30}));
  assert((out.depot_ids == std::vector<std::uint32_t>{11}));
}

void TestIdentity() {
  PackageContents out;
  assert(!Parse(Package("1234", {}), 1235, &out));
  assert(Parse(Package("1234", {}), 1234, &out));
  assert(out.app_ids.empty() && out.depot_ids.empty());

  Buffer two = Header();
  Begin(&two, "1234");
  End(&two);
  Begin(&two, "5");
  End(&two);
  End(&two);
  assert(!Parse(two, 1234, &out));

  Buffer trailing = Package("1234", {});
  trailing.push_back(0);
  assert(!Parse(trailing, 1234, &out));

  // Package id key present but not a section.
  Buffer scalar = Header();
  Int32(&scalar, "1234", 1);
  End(&scalar);
  assert(!Parse(scalar, 1234, &out));
}

void TestMalformed() {
  PackageContents out;
  Buffer good = Package("1234", AppIdsBody([] {
                          Buffer e;
                          Int32(&e, "0", 1);
                          return e;
                        }()));
  assert(Parse(good, 1234, &out));
  for (std::size_t cut = 0; cut < good.size(); ++cut) {
    assert(!ParsePackageBuffer(good.data(), cut, 1234, &out));
  }

  Buffer unknown;
  unknown.push_back(5);
  Str(&unknown, "k");
  Le(&unknown, 0, 4);
  assert(!Parse(Package("1234", unknown), 1234, &out));

  Buffer dup;
  Int32(&dup, "Key", 1);
  Int32(&dup, "kEY", 2);
  assert(!Parse(Package("1234", dup), 1234, &out));
  Buffer distinct;
  Int32(&distinct, "Key", 1);
  Int32(&distinct, "Other", 2);
  assert(Parse(Package("1234", distinct), 1234, &out));

  Buffer no_nul = Header();
  no_nul.push_back(1);
  no_nul.insert(no_nul.end(), {'a', 'b', 'c'});
  assert(!Parse(no_nul, 1234, &out));

  Buffer bad_key;
  bad_key.push_back(1);
  bad_key.insert(bad_key.end(), {'a', static_cast<std::uint8_t>(0xFF), 0});
  Str(&bad_key, "v");
  assert(!Parse(Package("1234", bad_key), 1234, &out));
  Buffer overlong;
  overlong.push_back(1);
  overlong.insert(overlong.end(), {static_cast<std::uint8_t>(0xC0), static_cast<std::uint8_t>(0x80), 0});
  Str(&overlong, "v");
  assert(!Parse(Package("1234", overlong), 1234, &out));
  Buffer good_utf8;
  String(&good_utf8, "caf\xC3\xA9", "\xE2\x82\xAC");
  assert(Parse(Package("1234", good_utf8), 1234, &out));

  Buffer bad_header = Package("1234", {});
  bad_header[0] = 2;
  assert(!Parse(bad_header, 1234, &out));
  assert(!ParsePackageBuffer(good.data(), 4, 1234, &out));
}

// Builds {"1234": {"n": {"n": ... }}} where the innermost section is at `depth`.
Buffer Nested(int deepest_depth) {
  Buffer b = Header();
  // Root section is depth 0; the package section is depth 1.
  for (int d = 1; d <= deepest_depth; ++d) Begin(&b, d == 1 ? "1234" : "n");
  for (int d = 1; d <= deepest_depth; ++d) End(&b);
  End(&b);
  return b;
}

void TestDepth() {
  PackageContents out;
  assert(Parse(Nested(31), 1234, &out));
  assert(!Parse(Nested(32), 1234, &out));
}

void TestIds() {
  PackageContents out;
  for (const char* bad : {"abc", "-1", "4294967296", "", "+5", "5 ", "0x10", "1.5"}) {
    Buffer e;
    String(&e, "0", bad);
    assert(!Parse(Package("1234", AppIdsBody(e)), 1234, &out));
  }
  Buffer max;
  String(&max, "0", "4294967295");
  assert(Parse(Package("1234", AppIdsBody(max)), 1234, &out));
  assert((out.app_ids == std::vector<std::uint32_t>{4294967295U}));

  Buffer u64;
  Int64(&u64, "0", 4000000000ULL);
  assert(Parse(Package("1234", AppIdsBody(u64)), 1234, &out));
  assert((out.app_ids == std::vector<std::uint32_t>{4000000000U}));
  Buffer u64_big;
  Int64(&u64_big, "0", 0x100000000ULL);
  assert(!Parse(Package("1234", AppIdsBody(u64_big)), 1234, &out));

  Buffer flt;
  Float(&flt, "0", 0x3f800000);
  assert(!Parse(Package("1234", AppIdsBody(flt)), 1234, &out));

  Buffer nested;
  Begin(&nested, "0");
  End(&nested);
  assert(!Parse(Package("1234", AppIdsBody(nested)), 1234, &out));

  // appids that is not a section is treated as empty.
  Buffer scalar;
  Int32(&scalar, "appids", 7);
  assert(Parse(Package("1234", scalar), 1234, &out));
  assert(out.app_ids.empty());
}

void TestSize() {
  PackageContents out;
  Buffer huge(8U * 1024U * 1024U + 1U, 0);
  huge[0] = 1;
  assert(!ParsePackageBuffer(huge.data(), huge.size(), 1234, &out));
  Buffer tiny = {1, 0, 0, 0};
  assert(!Parse(tiny, 1234, &out));
}

}  // namespace

int main() {
  TestNormal();
  TestIdentity();
  TestMalformed();
  TestDepth();
  TestIds();
  TestSize();
  std::puts("steam_binary_kv_test: ok");
  return 0;
}
