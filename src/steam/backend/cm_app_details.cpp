#include "steam/backend/cm_app_details.h"

#include "steam/backend/app_info.h"

namespace z3cloudplay::steam {

AppDetails DetailsFromAppInfo(GameId id, const std::string& buffer) {
  AppDetails details;
  details.id = id;
  const auto app = ParseAppInfo(id, buffer);
  if (!app) {
    details.reason = "Steam returned unreadable details for this game.";
    return details;
  }
  details.available = true;
  std::vector<DepotInfo> depots;
  std::string reason;
  const std::set<std::uint32_t> owned{id};
  if (!SelectDepots(*app, owned, std::nullopt, &depots, &reason)) {
    details.reason = "No Windows content to install.";
    return details;
  }
  std::uint64_t install = 0;
  std::uint64_t download = 0;
  for (const auto& depot : depots) {
    install += depot.size;
    // Some appinfo reports a download larger than the install (FINAL FANTASY XIII, issue #5); compressed chunks never are.
    download += depot.size && depot.download_size > depot.size ? depot.size : depot.download_size;
  }
  if (install) details.install_bytes = install;
  if (download) details.download_bytes = download;
  std::vector<AppLaunch> launches;
  details.windows_installable = EligibleLaunches(*app, owned, &launches, &reason);
  if (!details.windows_installable) details.reason = "No Windows launch configuration.";
  for (const auto& launch : launches)
    details.launches.push_back({launch.executable, launch.arguments, launch.working_directory,
                                launch.description.value_or(std::string())});
  return details;
}

std::vector<AppDetails> CmAppDetailsSource::Fetch(const StoredSession& session,
                                                  const std::vector<GameId>& ids,
                                                  const std::atomic<bool>& cancel) {
  std::vector<std::uint32_t> app_ids(ids.begin(), ids.end());
  if (app_ids.size() > kMaxAppInfoBatch) app_ids.resize(kMaxAppInfoBatch);
  const auto fetched = client_.FetchAppInfo(session, app_ids, cancel);
  std::vector<AppDetails> result;
  if (!fetched.ok()) return result;  // the service reports these as retryable
  for (const auto id : ids) {
    const auto found = fetched.buffers.find(id);
    if (found == fetched.buffers.end()) {
      AppDetails missing;
      missing.id = id;
      missing.reason = "Steam has no details for this game.";
      result.push_back(std::move(missing));
      continue;
    }
    result.push_back(DetailsFromAppInfo(id, found->second));
  }
  return result;
}

}  // namespace z3cloudplay::steam
