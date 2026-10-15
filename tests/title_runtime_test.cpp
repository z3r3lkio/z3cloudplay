#include "platform/ps5/title_runtime.h"
#include <cstdlib>
#include <cstdio>
#include <cstddef>
#include <vector>
using namespace z3cloudplay::ps5;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace {
std::vector<int> calls;
int init_result = 0, user_result = 0;
uint32_t launch_result = 0x1234;
int Initialize() { calls.push_back(1); return init_result; }
int Foreground(uint32_t* user) { calls.push_back(2); *user = 123; return user_result; }
uint32_t Launch(const char* title, const char** argv, LaunchParameters* p) {
  calls.push_back(3);
  CHECK(std::string(title) == "PPSA99995");
  CHECK(p->size == 32 && p->user_id == 123 && p->check_flag == 0);
  CHECK(argv && std::string(argv[0]) == "profile=z3cloudplay-steam-220" && !argv[1]);
  return launch_result;
}
int Load(uint32_t id) { CHECK(id == 0x80000010u); return 0; }
int Handle(uint32_t id, int32_t* handle) { CHECK(id == 0x80000010u); *handle = 12; return 0; }
int Resolve(int32_t handle, const char* symbol, void** address) {
  CHECK(handle == 12);
  *address = std::string(symbol) == "sceLncUtilInitialize" ? reinterpret_cast<void*>(Initialize)
      : std::string(symbol) == "sceLncUtilLaunchApp" ? reinterpret_cast<void*>(Launch) : nullptr;
  return *address ? 0 : -1;
}
uint32_t PublicLaunch(const char* title, const char** argv, void* params) {
  calls.push_back(4);
  CHECK(std::string(title) == "PPSA99995" && params != nullptr);
  const auto* p = static_cast<LaunchParameters*>(params);
  CHECK(p->size == 32 && p->user_id == 123 && p->check_flag == 1);
  CHECK(argv && std::string(argv[0]) == "profile=z3cloudplay-steam-220" && !argv[1]);
  return launch_result;
}
int PublicResolve(int32_t handle, const char* symbol, void** address) {
  CHECK(handle == 12);
  *address = std::string(symbol) == "sceSystemServiceLaunchApp" ? reinterpret_cast<void*>(PublicLaunch) : nullptr;
  return *address ? 0 : -1;
}
}
int main() {
  auto native = ResolveTitleApi({Load, Handle, Resolve}, Foreground);
  CHECK(native.initialize == Initialize && native.launch == Launch);
  CHECK(!ResolveTitleApi({}, Foreground).launch);
  CHECK(sizeof(LaunchParameters) == 32 && offsetof(LaunchParameters, crash_report) == 16);
  TitleBackend backend({Initialize, Foreground, Launch});
  auto r = backend.Launch("bad", {"profile=z3cloudplay-steam-220"});
  CHECK(!r.ok && calls.empty());
  r = backend.Launch("PPSA99995", {"profile=z3cloudplay-steam-220"});
  CHECK(r.ok && r.code == 0x1234 && calls == std::vector<int>({1,2,3}));
  calls.clear(); init_result = static_cast<int>(0x80940004u);
  TitleBackend failed({Initialize, Foreground, Launch});
  r = failed.Launch("PPSA99995");
  CHECK(!r.ok && r.stage == "initialize" && calls == std::vector<int>({1}));
  calls.clear(); init_result = static_cast<int>(0x80940018u); user_result = -1;
  TitleBackend no_user({Initialize, Foreground, Launch});
  r = no_user.Launch("PPSA99995");
  CHECK(!r.ok && r.stage == "foreground-user" && calls == std::vector<int>({1,2}));
  calls.clear(); user_result = 0; launch_result = 0x80940005u;
  r = backend.Launch("PPSA99995", {"profile=z3cloudplay-steam-220"});
  CHECK(!r.ok && r.code == launch_result && calls == std::vector<int>({2,3}));
  TitleBackend missing({});
  CHECK(!missing.Launch("PPSA99995").ok);
  calls.clear(); launch_result = 0;
  auto public_api = ResolveTitleApi({Load, Handle, PublicResolve}, Foreground);
  CHECK(public_api.public_launch == PublicLaunch);
  TitleBackend public_backend(public_api);
  r = public_backend.Launch("PPSA99995", {"profile=z3cloudplay-steam-220"});
  CHECK(r.ok && r.stage == "system-service" && calls == std::vector<int>({2,4}));
  calls.clear(); launch_result = 0x80940005u;
  r = public_backend.Launch("PPSA99995", {"profile=z3cloudplay-steam-220"});
  CHECK(!r.ok && r.code == launch_result && calls == std::vector<int>({2,4}));
  std::puts("title runtime tests passed");
}
