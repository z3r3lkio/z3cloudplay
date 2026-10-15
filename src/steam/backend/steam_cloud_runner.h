#pragma once

#include "steam/backend/cloud_sync.h"
#include "steam/cloud_service.h"

#include <functional>
#include <map>

namespace z3cloudplay::steam {

// CloudRunner over the Steam Cloud engine. A game's Wine prefix (for Windows user folders) lives
// at the explicitly supplied bottle path, with a legacy state-folder fallback.
class SteamCloudRunner final : public CloudRunner {
 public:
  SteamCloudRunner(CmClient& cm, HttpClient& http);
  explicit SteamCloudRunner(cloud::CloudHooks hooks);

  CloudReport Check(const StoredSession& session, const CloudLocation& location, const std::atomic<bool>& cancel) override;
  CloudReport Apply(const StoredSession& session, const CloudLocation& location, CloudResolution resolution,
                    bool confirm_account, const std::atomic<bool>& cancel) override;
  std::vector<CloudDrift> QuickCheck(const StoredSession& session, const std::vector<CloudLocation>& locations,
                                     const std::atomic<bool>& cancel) override;
  bool RemoveLocal(const CloudLocation& location, std::string* message) override;
  void SetProgress(std::function<void(std::uint64_t done, std::uint64_t total)> progress) override { progress_ = std::move(progress); }

  // Optional sanitized log: the Steam app ID, what happened, and a short user-facing message.
  void SetLog(std::function<void(GameId app, const char* event, const std::string& message)> log);

  static cloud::GameSaveLocation SaveLocation(const CloudLocation& location);

 private:
  cloud::CloudSync sync_;
  struct PendingPlan {
    cloud::CloudPlan plan;
    cloud::GameSaveLocation location;
  };
  std::map<GameId, PendingPlan> plans_;  // bind Apply to the location actually checked
  std::function<void(GameId, const char*, const std::string&)> log_;
  GameId current_app_ = 0;
  std::function<void(std::uint64_t, std::uint64_t)> progress_;  // the game being checked or synced, for traces
};

}  // namespace z3cloudplay::steam
