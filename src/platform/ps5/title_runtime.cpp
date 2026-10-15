#include "platform/ps5/title_runtime.h"
#include <cstddef>
#include <cerrno>

namespace z3cloudplay::ps5 {
static_assert(sizeof(LaunchParameters) == 32);
static_assert(offsetof(LaunchParameters, crash_report) == 16);
static_assert(offsetof(LaunchParameters, check_flag) == 24);
TitleApi ResolveTitleApi(ModuleApi modules, int (*foreground_user)(std::uint32_t*)) {
  if (!modules.load_internal || !modules.handle_internal || !modules.resolve) return {};
  constexpr std::uint32_t system_service = 0x80000010u;
  modules.load_internal(system_service);
  std::int32_t handle = -1;
  if (modules.handle_internal(system_service, &handle) != 0 || handle < 0) return {};
  void* initialize = nullptr;
  void* launch = nullptr;
  if (modules.resolve(handle, "sceLncUtilInitialize", &initialize) != 0 || !initialize ||
      modules.resolve(handle, "sceLncUtilLaunchApp", &launch) != 0 || !launch) {
    void* public_launch = nullptr;
    if (modules.resolve(handle, "sceSystemServiceLaunchApp", &public_launch) != 0 || !public_launch) return {};
    return {nullptr, foreground_user, nullptr, reinterpret_cast<decltype(TitleApi::public_launch)>(public_launch)};
  }
  return {reinterpret_cast<decltype(TitleApi::initialize)>(initialize), foreground_user,
          reinterpret_cast<decltype(TitleApi::launch)>(launch)};
}
bool ValidTitleId(const std::string& title) {
  if (title.size() != 9) return false;
  for (std::size_t i = 0; i < title.size(); ++i)
    if (i < 4 ? title[i] < 'A' || title[i] > 'Z' : title[i] < '0' || title[i] > '9') return false;
  return true;
}
TitleResult TitleBackend::Launch(const std::string& title, const std::vector<std::string>& arguments) {
  if (!ValidTitleId(title)) return {false, EINVAL, "title-id"};
  if (arguments.size() > 8) return {false, E2BIG, "arguments"};
  for (const auto& arg : arguments)
    if (arg.size() > 512 || arg.find('\0') != std::string::npos) return {false, EINVAL, "arguments"};
  std::vector<const char*> argv;
  for (const auto& arg : arguments) argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  if (api_.public_launch) {
    if (!api_.foreground_user) return {false, ENOSYS, "provider"};
    std::uint32_t user = 0;
    const auto user_code = static_cast<std::uint32_t>(api_.foreground_user(&user));
    if (user_code) return {false, user_code, "foreground-user"};
    // Native big-app handoff requires SkipLaunchCheck after the title's root grant.
    LaunchParameters params{sizeof(LaunchParameters), user, 0, 0, 1};
    const auto code = api_.public_launch(title.c_str(), arguments.empty() ? nullptr : argv.data(), &params);
    return {!(code & 0x80000000u), code, "system-service"};
  }
  if (!api_.initialize || !api_.foreground_user || !api_.launch) return {false, ENOSYS, "provider"};
  if (!initialized_) {
    const auto code = static_cast<std::uint32_t>(api_.initialize());
    if (code && code != 0x80940018u) return {false, code, "initialize"};
    initialized_ = true;
  }
  std::uint32_t user = 0;
  const auto user_code = static_cast<std::uint32_t>(api_.foreground_user(&user));
  if (user_code) return {false, user_code, "foreground-user"};
  LaunchParameters params{sizeof(LaunchParameters), user, 0, 0, 0};
  const auto code = api_.launch(title.c_str(), arguments.empty() ? nullptr : argv.data(), &params);
  return {!(code & 0x80000000u), code, "launch"};
}
}  // namespace z3cloudplay::ps5
