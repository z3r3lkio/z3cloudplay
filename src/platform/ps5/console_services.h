#pragma once
// console_services.h - Small PS5 console adapters for the runtime host.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include <optional>
#include <string>

namespace z3cloudplay::ps5 {
// Running kernel firmware version as "MM.mm" (for example "13.60"), or nothing.
std::optional<std::string> ConsoleFirmwareVersion();
// Raw values from the firmware sources, for diagnostics.
std::string ConsoleFirmwareDiagnostics();
// POSTs JSON to the console-local ShadowMountPlus API (127.0.0.1:10101). Returns
// the response body, including error bodies, or nothing on transport failure.
// Worker-only: it blocks for at most five seconds.
std::optional<std::string> ShadowMountPost(const std::string& route, const std::string& body);
}  // namespace z3cloudplay::ps5
