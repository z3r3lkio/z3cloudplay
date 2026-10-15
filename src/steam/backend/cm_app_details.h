#pragma once

#include "steam/app_details_service.h"
#include "steam/backend/cm_client.h"

namespace z3cloudplay::steam {

// App details from PICS: totals of the depots Z3CloudPlay would install (Windows, English, public
// branch) and whether a Windows launch entry exists. Sizes are Steam's per-depot figures, so they
// are an estimate until the manifests are fetched.
class CmAppDetailsSource final : public AppDetailsSource {
 public:
  explicit CmAppDetailsSource(CmClient& client) : client_(client) {}
  std::vector<AppDetails> Fetch(const StoredSession& session, const std::vector<GameId>& ids,
                                const std::atomic<bool>& cancel) override;

 private:
  CmClient& client_;
};

// Pure part, for tests: turns one parsed app buffer into details.
AppDetails DetailsFromAppInfo(GameId id, const std::string& buffer);

}  // namespace z3cloudplay::steam
