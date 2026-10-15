#pragma once
#include "runtime/install_service.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace z3cloudplay::ps5 {
struct ProcessTitleIdentity { std::uint32_t app_id = 0; std::string title_id; };
struct TitleActivityApi {
  // Complete kernel process inventory; failure is never an empty inventory.
  std::function<std::optional<std::vector<std::int32_t>>()> processes;
  std::function<std::optional<ProcessTitleIdentity>(std::int32_t)> identity;
};
// FW13.60 kinfo_proc ABI: native PID at +72, length at +0. Reject malformed,
// partial, duplicate and mixed-layout records before querying title identities.
std::optional<std::vector<std::int32_t>> DecodeTitleActivityProcessIds(const std::vector<std::uint8_t>& bytes);
// A positively identified target is active; stopped requires complete valid
// identities twice with unchanged enclosing inventories. Unknown never authorizes edits.
runtime::NativeActivity QueryTitleActivity(const std::string& title_id, const TitleActivityApi& api);
// Read-only native boundary. Host builds always return unknown. FW13.60 native
// ABI/API qualification is required before production uses this for mutation.
using TitleActivityTrace = std::function<void(const std::string&)>;
runtime::NativeActivity ReadTitleActivity(const std::string& title_id, const TitleActivityTrace& trace = {});
}  // namespace z3cloudplay::ps5
