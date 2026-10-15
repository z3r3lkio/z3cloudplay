#include "platform/ps5/title_activity.h"
#include "platform/ps5/title_runtime.h"
#include <algorithm>
#include <cstring>
#include <set>
#if defined(__PROSPERO__)
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#include <cstddef>
extern "C" int sceKernelDlsym(int, const char*, void**);
#endif
namespace z3cloudplay::ps5 {
namespace {
constexpr std::size_t kMaxBytes = 4 * 1024 * 1024, kMaxProcesses = 4096;
bool InventoryValid(const std::vector<std::int32_t>& pids) {
  if (pids.empty() || pids.size() > kMaxProcesses) return false;
  std::set<std::int32_t> unique;
  bool user = false;
  for (auto pid : pids) { if (pid < 0 || !unique.insert(pid).second) return false; user |= pid > 0; }
  return user;
}
}
std::optional<std::vector<std::int32_t>> DecodeTitleActivityProcessIds(const std::vector<std::uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > kMaxBytes) return std::nullopt;
  std::vector<std::int32_t> pids; std::size_t offset = 0; std::uint32_t stride = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 76) return std::nullopt;
    std::uint32_t length = 0; std::int32_t pid = -1;
    std::memcpy(&length, bytes.data() + offset, 4); std::memcpy(&pid, bytes.data() + offset + 72, 4);
    if (length < 76 || length > 8192 || length > bytes.size() - offset || (stride && stride != length)) return std::nullopt;
    stride = length; pids.push_back(pid); if (pids.size() > kMaxProcesses) return std::nullopt;
    offset += length;
  }
  return InventoryValid(pids) ? std::optional<std::vector<std::int32_t>>(std::move(pids)) : std::nullopt;
}
runtime::NativeActivity QueryTitleActivity(const std::string& title, const TitleActivityApi& api) {
  using runtime::NativeActivity;
  if (!ValidTitleId(title) || !api.processes || !api.identity) return NativeActivity::kUnknown;
  auto first = api.processes(); if (!first || !InventoryValid(*first)) return NativeActivity::kUnknown;
  std::sort(first->begin(), first->end());
  std::vector<ProcessTitleIdentity> identities(first->size());
  bool complete = true;
  auto valid_identity = [](const std::optional<ProcessTitleIdentity>& value) {
    // FW13.60 PID 1 has app_id zero and a valid system title. A successful
    // title identity is authoritative regardless of app_id; only an untitled
    // identity requires app_id zero. Failed calls never become non-apps.
    return value && ((value->app_id == 0 && value->title_id.empty()) || ValidTitleId(value->title_id));
  };
  for (std::size_t i = 0; i < first->size(); ++i) {
    const auto pid = (*first)[i]; if (pid == 0) continue; // Kernel cannot be a user title.
    auto identity = api.identity(pid);
    if (!valid_identity(identity)) { complete = false; continue; }
    if (identity->title_id == title) return NativeActivity::kActive;
    identities[i] = *identity;
  }
  if (!complete) return NativeActivity::kUnknown;
  auto second = api.processes(); if (!second || !InventoryValid(*second)) return NativeActivity::kUnknown;
  std::sort(second->begin(), second->end()); if (*first != *second) return NativeActivity::kUnknown;
  for (std::size_t i = 0; i < second->size(); ++i) {
    if ((*second)[i] == 0) continue;
    auto identity = api.identity((*second)[i]);
    if (!valid_identity(identity)) { complete = false; continue; }
    if (identity->title_id == title) return NativeActivity::kActive;
    if (identity->app_id != identities[i].app_id || identity->title_id != identities[i].title_id) complete = false;
  }
  if (!complete) return NativeActivity::kUnknown;
  auto final = api.processes(); if (!final || !InventoryValid(*final)) return NativeActivity::kUnknown;
  std::sort(final->begin(), final->end());
  return *first == *final ? NativeActivity::kStopped : NativeActivity::kUnknown;
}
runtime::NativeActivity ReadTitleActivity(const std::string& title, const TitleActivityTrace& trace) {
  if (!ValidTitleId(title)) return runtime::NativeActivity::kUnknown;
#if defined(__PROSPERO__)
  // Same 96-byte AppInfo layout used by the title-scoped elevation helper.
  struct AppInfo { std::uint32_t app_id; std::uint64_t reserved; char title_id[14]; char padding[0x3c]; };
  static_assert(sizeof(AppInfo) == 96 && offsetof(AppInfo, title_id) == 16);
  static_assert(offsetof(kinfo_proc, ki_pid) == 72 && offsetof(kinfo_proc, ki_structsize) == 0);
  using GetInfo = int (*)(int, AppInfo*); GetInfo get_info = nullptr;
  if (sceKernelDlsym(0x2001, "sceKernelGetAppInfo", reinterpret_cast<void**>(&get_info)) != 0 || !get_info)
    return runtime::NativeActivity::kUnknown;
  auto identity = [get_info, &trace](std::int32_t pid) -> std::optional<ProcessTitleIdentity> {
    AppInfo value{};
    const auto rc = get_info(pid, &value);
    const auto* end = static_cast<const char*>(std::memchr(value.title_id, 0, sizeof(value.title_id)));
    if (trace) trace("identity pid=" + std::to_string(pid) + " rc=" + std::to_string(static_cast<std::uint32_t>(rc)) +
        " app_id=" + std::to_string(value.app_id) + " title=" + (end ? std::string(value.title_id, end - value.title_id) : "<unterminated>"));
    if (rc != 0) return std::nullopt;
    if (!end) return std::nullopt;
    return ProcessTitleIdentity{value.app_id, std::string(value.title_id, static_cast<std::size_t>(end - value.title_id))};
  };
  const auto self_pid = ::getpid(); auto self = self_pid > 0 ? identity(self_pid) : std::nullopt;
  if (!self || !self->app_id || !ValidTitleId(self->title_id)) return runtime::NativeActivity::kUnknown;
  TitleActivityApi api; api.identity = identity;
  api.processes = [self_pid, &trace]() -> std::optional<std::vector<std::int32_t>> {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}; std::size_t length = 0;
    if (::sysctl(mib, 4, nullptr, &length, nullptr, 0) != 0 || !length || length > kMaxBytes) return std::nullopt;
    std::vector<std::uint8_t> bytes(length);
    if (::sysctl(mib, 4, bytes.data(), &length, nullptr, 0) != 0 || length > bytes.size() || !length) return std::nullopt;
    bytes.resize(length); auto pids = DecodeTitleActivityProcessIds(bytes);
    if (trace) trace("snapshot bytes=" + std::to_string(bytes.size()) + " processes=" + (pids ? std::to_string(pids->size()) : "invalid"));
    // Qualify the snapshot's PID field against this independently known PID.
    if (!pids || std::find(pids->begin(), pids->end(), self_pid) == pids->end()) return std::nullopt;
    return pids;
  };
  return QueryTitleActivity(title, api);
#else
  (void)title; (void)trace; return runtime::NativeActivity::kUnknown;
#endif
}
}  // namespace z3cloudplay::ps5
