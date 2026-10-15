#include "platform/ps5/title_activity.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace z3cloudplay::ps5;
using z3cloudplay::runtime::NativeActivity;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
std::vector<std::uint8_t> Records(std::initializer_list<int> pids) {
  std::vector<std::uint8_t> bytes(pids.size() * 1088); std::size_t offset = 0;
  for (int pid : pids) { std::uint32_t length = 1088; std::memcpy(bytes.data() + offset, &length, 4); std::memcpy(bytes.data() + offset + 72, &pid, 4); offset += length; }
  return bytes;
}
int main() {
  auto decoded = DecodeTitleActivityProcessIds(Records({0, 100, 201})); CHECK(decoded && *decoded == std::vector<std::int32_t>({0, 100, 201}));
  for (unsigned damage = 0; damage < 6; ++damage) {
    auto bytes = Records({0, 100, 201});
    if (damage == 0) bytes.pop_back();
    if (damage == 1) { std::uint32_t zero = 0; std::memcpy(bytes.data(), &zero, 4); }
    if (damage == 2) { int pid = -1; std::memcpy(bytes.data() + 72, &pid, 4); }
    if (damage == 3) { int pid = 100; std::memcpy(bytes.data() + 1088 * 2 + 72, &pid, 4); }
    if (damage == 4) bytes.push_back(0);
    if (damage == 5) { std::uint32_t length = 1084; std::memcpy(bytes.data() + 1088, &length, 4); }
    CHECK(!DecodeTitleActivityProcessIds(bytes));
  }
  CHECK(!DecodeTitleActivityProcessIds({}));
  TitleActivityApi api;
  api.processes = [] { return std::optional<std::vector<std::int32_t>>({{0, 100, 201}}); };
  api.identity = [](int pid) { return std::optional<ProcessTitleIdentity>({pid == 201 ? 123u : 0u, pid == 201 ? "FAKE78001" : ""}); };
  CHECK(QueryTitleActivity("FAKE78001", api) == NativeActivity::kActive);
  CHECK(QueryTitleActivity("FAKE78002", api) == NativeActivity::kStopped);
  unsigned identities = 0;
  api.identity = [&](int pid) { return std::optional<ProcessTitleIdentity>({pid == 201 ? 123u : 0u,
      pid == 201 ? (++identities == 1 ? "FAKE78002" : "FAKE78001") : ""}); };
  CHECK(QueryTitleActivity("FAKE78001", api) == NativeActivity::kActive);
  api.identity = [](int) { return std::optional<ProcessTitleIdentity>{}; };
  CHECK(QueryTitleActivity("FAKE78002", api) == NativeActivity::kUnknown);
  api.identity = [](int) { return std::optional<ProcessTitleIdentity>({123, ""}); };
  CHECK(QueryTitleActivity("FAKE78002", api) == NativeActivity::kUnknown);
  api.identity = [](int) { return std::optional<ProcessTitleIdentity>({0, "FAKE78001"}); };
  CHECK(QueryTitleActivity("FAKE78001", api) == NativeActivity::kActive);
  // FW13.60 PID 1 is a successful titled system identity with app_id zero.
  api.identity = [](int pid) { return std::optional<ProcessTitleIdentity>({pid == 201 ? 123u : 0u, pid == 201 ? "FAKE77031" : "NPXS45095"}); };
  CHECK(QueryTitleActivity("FAKE78001", api) == NativeActivity::kStopped);
  CHECK(QueryTitleActivity("NPXS45095", api) == NativeActivity::kActive);
  api.identity = [](int) { return std::optional<ProcessTitleIdentity>({0, ""}); };
  unsigned reads = 0;
  api.processes = [&] { return std::optional<std::vector<std::int32_t>>({{100, ++reads == 1 ? 201 : 202}}); };
  CHECK(QueryTitleActivity("FAKE78002", api) == NativeActivity::kUnknown);
  api.processes = [] { return std::optional<std::vector<std::int32_t>>({{100, 100}}); };
  CHECK(QueryTitleActivity("FAKE78002", api) == NativeActivity::kUnknown);
  CHECK(QueryTitleActivity("../unsafe", api) == NativeActivity::kUnknown);
  CHECK(ReadTitleActivity("FAKE78002") == NativeActivity::kUnknown);
  std::puts("title activity tests passed");
}
