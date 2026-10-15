#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace z3cloudplay::ps5 {
struct LaunchParameters {
  std::uint32_t size, user_id, app_options;
  std::uint64_t crash_report;
  std::uint32_t check_flag;
};
struct TitleResult {
  bool ok = false;
  std::uint32_t code = 0;
  std::string stage;
};
// Injected native boundary: portable tests exercise sequencing and failure propagation.
struct TitleApi {
  int (*initialize)() = nullptr;
  int (*foreground_user)(std::uint32_t*) = nullptr;
  std::uint32_t (*launch)(const char*, const char**, LaunchParameters*) = nullptr;
  std::uint32_t (*public_launch)(const char*, const char**, void*) = nullptr;
};
struct ModuleApi {
  int (*load_internal)(std::uint32_t) = nullptr;
  int (*handle_internal)(std::uint32_t, std::int32_t*) = nullptr;
  int (*resolve)(std::int32_t, const char*, void**) = nullptr;
};
TitleApi ResolveTitleApi(ModuleApi modules, int (*foreground_user)(std::uint32_t*));
bool ValidTitleId(const std::string& title_id);
class TitleBackend {
 public:
  explicit TitleBackend(TitleApi api) : api_(api) {}
  TitleResult Launch(const std::string& title_id, const std::vector<std::string>& arguments = {});
 private:
  TitleApi api_;
  bool initialized_ = false;
};
}  // namespace z3cloudplay::ps5
