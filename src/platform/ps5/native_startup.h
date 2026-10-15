#pragma once

#include "platform/ps5/native_access.h"

namespace z3cloudplay::ps5 {
// Probes linked graphics entry points, requests the title-scoped grant, then
// initializes UserService before returning to the UI/native launch services.
// Display initialization must follow bootstrap to preserve helper eligibility.
// All services created afterwards must use the returned paths.
NativeAccessResult PrepareNativeStartup(const std::string& title_id);
}
