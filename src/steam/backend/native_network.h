#pragma once

#include <curl/curl.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace z3cloudplay::steam::internal {

// Minimal single-threaded initialization for the title-scoped grant transport.
// Does not run the watchdog-based nonblocking preflight. Steam/curl callers must
// still call InitializeNativeNetwork after bootstrap to complete that check.
bool InitializeNativeNetworkForBootstrap();

// Initializes and semantically checks the process-wide native network runtime before curl global
// initialization. The PS5 runtime is intentionally retained until process exit so this adapter
// never terminates networking initialized or used by another subsystem.
bool InitializeNativeNetwork();

// Resolves the URL host through the bounded PS5 resolver and attaches the numeric address through
// CURLOPT_RESOLVE. Host builds use curl's normal resolver and leave entries null.
enum class NativeResolveError {
  kNone,
  kFailed,
  kTimeout,
  kCanceled,
};

NativeResolveError AttachNativeResolution(CURL* easy, const std::string& url,
                                          std::uint32_t timeout_ms,
                                          const std::atomic<bool>& cancel,
                                          curl_slist** entries);

}  // namespace z3cloudplay::steam::internal
