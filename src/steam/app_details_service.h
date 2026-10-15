#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace z3cloudplay::steam {

// One Windows launch option from Steam's app info, as Steam wrote it.
struct LaunchOption {
  std::string executable;
  std::string arguments;
  std::string working_directory;
  std::string description;
};

// What a game's Steam app info says about installing it on this console.
struct AppDetails {
  GameId id = 0;
  std::uint64_t account_generation = 0;
  bool available = false;  // false: Steam had no usable app info (see reason)
  std::optional<std::uint64_t> download_bytes;
  std::optional<std::uint64_t> install_bytes;
  bool windows_installable = false;  // depots and a launch entry exist for Windows/English
  std::string reason;                // short, user-facing, no identifiers
  std::vector<LaunchOption> launches;  // Windows launch options, default first
};

// Runs on the service worker; may block on the network.
class AppDetailsSource {
 public:
  virtual ~AppDetailsSource() = default;
  virtual std::vector<AppDetails> Fetch(const StoredSession& session, const std::vector<GameId>& ids,
                                        const std::atomic<bool>& cancel) = 0;
};

// Fetches app details for games the player opens. Requests are deduplicated and cached for the
// current account generation; Invalidate drops everything for a new account. The source and the
// repository outlive the service.
class AppDetailsService {
 public:
  AppDetailsService(AppDetailsSource& source, SessionRepository& sessions);
  ~AppDetailsService();
  AppDetailsService(const AppDetailsService&) = delete;
  AppDetailsService& operator=(const AppDetailsService&) = delete;

  void Request(GameId id, std::uint64_t account_generation);
  void Invalidate(std::uint64_t account_generation);
  std::vector<AppDetails> Pump(std::size_t maximum = 8);
  void Shutdown();

  static constexpr std::size_t kMaxPending = 64;
  static constexpr std::size_t kBatch = 8;

 private:
  void Run();

  AppDetailsSource& source_;
  SessionRepository& sessions_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::uint64_t generation_ = 0;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;  // queued, in flight or answered for this generation
  std::deque<AppDetails> results_;
  std::atomic<bool> cancel_{false};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace z3cloudplay::steam
