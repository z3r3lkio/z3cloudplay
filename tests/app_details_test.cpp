#include "steam/app_details_service.h"
#include "steam/backend/cm_app_details.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>

using namespace z3cloudplay;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;

namespace {

class Sessions final : public SessionRepository {
 public:
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(StoredSession{123, "synthetic", "refresh", "access", {}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};

class Source final : public AppDetailsSource {
 public:
  std::mutex mutex;
  std::vector<std::vector<GameId>> calls;
  std::atomic<bool> fail{false};
  std::vector<AppDetails> Fetch(const StoredSession&, const std::vector<GameId>& ids,
                                const std::atomic<bool>&) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      calls.push_back(ids);
    }
    if (fail) return {};
    std::vector<AppDetails> result;
    for (const auto id : ids) result.push_back({id, 0, true, id * 10ull, id * 100ull, true, {}});
    return result;
  }
};

std::vector<AppDetails> Await(AppDetailsService& service, std::size_t count) {
  std::vector<AppDetails> all;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (all.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto& details : service.Pump()) all.push_back(std::move(details));
    std::this_thread::sleep_for(1ms);
  }
  assert(all.size() >= count);
  return all;
}

void RequestsAreDeduplicatedPerGeneration() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Request(7, 1);
  service.Request(7, 1);
  const auto first = Await(service, 1);
  assert(first[0].id == 7 && first[0].available && first[0].install_bytes == 700u && first[0].account_generation == 1);
  service.Request(7, 1);
  std::this_thread::sleep_for(20ms);
  assert(service.Pump().empty());
  std::lock_guard<std::mutex> lock(source.mutex);
  assert(source.calls.size() == 1);
}

void NewGenerationDropsResultsAndRefetches() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Request(7, 1);
  Await(service, 1);
  service.Invalidate(2);
  service.Request(7, 2);
  const auto again = Await(service, 1);
  assert(again[0].account_generation == 2);
}

void FailuresAreRetryable() {
  Sessions sessions;
  Source source;
  source.fail = true;
  AppDetailsService service(source, sessions);
  service.Request(9, 1);
  const auto failed = Await(service, 1);
  assert(!failed[0].available && !failed[0].reason.empty());
  source.fail = false;
  service.Request(9, 1);
  const auto recovered = Await(service, 1);
  assert(recovered[0].available && recovered[0].download_bytes == 90u);
}

void AppInfoBecomesWindowsEstimate() {
  const std::string buffer = R"("appinfo"
{
  "appid" "440"
  "common" { "name" "Synthetic" "type" "game" }
  "config"
  {
    "installdir" "Synthetic"
    "launch"
    {
      "0" { "executable" "game.exe" "type" "default" "config" { "oslist" "windows" } }
      "1" { "executable" "bin\\safe.exe" "arguments" "-safe" "description" "Safe mode" "config" { "oslist" "windows" } }
      "2" { "executable" "game.app" "config" { "oslist" "macos" } }
    }
  }
  "depots"
  {
    "441" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "11" "size" "3000" "download" "1200" } } }
    "442" { "manifests" { "public" { "gid" "12" "size" "500" "download" "200" } } }
    "443" { "config" { "oslist" "macos" } "manifests" { "public" { "gid" "13" "size" "999" "download" "999" } } }
  }
})";
  const auto details = DetailsFromAppInfo(440, buffer + std::string(2, '\0'));
  assert(details.available && details.windows_installable);
  assert(details.install_bytes == 3500u && details.download_bytes == 1400u);
  // The Windows launch options, default first, are kept for preparing the game.
  assert(details.launches.size() == 2 && details.launches[0].executable == "game.exe");
  assert(details.launches[1].executable == "bin\\safe.exe" && details.launches[1].arguments == "-safe" &&
         details.launches[1].description == "Safe mode");
  const auto unreadable = DetailsFromAppInfo(440, "not vdf {");
  assert(!unreadable.available && !unreadable.reason.empty());
  const std::string mac_only = R"("appinfo" { "depots" { "1" { "config" { "oslist" "macos" } "manifests" { "public" { "gid" "1" } } } } })";
  const auto none = DetailsFromAppInfo(440, mac_only);
  assert(none.available && !none.windows_installable && !none.install_bytes);
}

// Half-Life 2's appinfo since its 20th anniversary update (trimmed): the public branch's Windows launch option is tagged
// betakey "default", and the other Windows options belong to the steam_legacy and test branches (issue #10).
void DefaultBetaKeyIsThePublicBranch() {
  const std::string buffer = R"("appinfo"
{
  "appid" "220"
  "common" { "name" "Half-Life 2" "type" "Game" }
  "config"
  {
    "installdir" "Half-Life 2"
    "launch"
    {
      "0" { "executable" "hl2.exe" "arguments" "-steam -game hl2_complete" "workingdir" "bin" "config" { "betakey" "default" "oslist" "windows" } }
      "1" { "executable" "hl2.sh" "arguments" "-game hl2 -steam" "config" { "oslist" "macos" } }
      "2" { "executable" "hl2.sh" "arguments" "-steam -game hl2_complete" "config" { "oslist" "linux" } }
      "3" { "executable" "hl2.exe" "arguments" "-steam -game hl2" "workingdir" "bin" "config" { "betakey" "steam_legacy" "oslist" "windows" } }
      "4" { "executable" "hl2.sh" "arguments" "-steam -game hl2" "config" { "betakey" "steam_legacy" "oslist" "linux" } }
      "5" { "executable" "hl2.exe" "arguments" "-steam -game hl2_complete" "workingdir" "bin" "config" { "betakey" "test" "oslist" "windows" } }
      "6" { "executable" "hl2.sh" "arguments" "-steam -game hl2_complete" "config" { "betakey" "test" "oslist" "linux" } }
    }
  }
  "depots"
  {
    "221" { "manifests" { "public" { "download" "2751430624" "gid" "4622516436473900521" "size" "5437140737" } } "systemdefined" "1" }
    "222" { "config" { "oslist" "windows" } "manifests" { "public" { "download" "205502384" "gid" "6432688940779947022" "size" "333424562" } } "systemdefined" "1" }
    "389" { "depotfromapp" "380" "sharedinstall" "2" }
  }
})";
  const auto details = DetailsFromAppInfo(220, buffer);
  assert(details.available && details.windows_installable);
  assert(details.launches.size() == 1 && details.launches[0].executable == "hl2.exe" &&
         details.launches[0].arguments == "-steam -game hl2_complete");
}

// FINAL FANTASY XIII's appinfo (trimmed) reports each depot's download as about 1,700 times its installed size, which
// showed 160 TB on Install (issue #5). A compressed download is never larger than what it installs.
void ImpossibleDownloadSizeFallsBackToInstallSize() {
  const std::string buffer = R"("appinfo"
{
  "appid" "292120"
  "common" { "name" "FINAL FANTASY XIII" "type" "Game" }
  "config" { "launch" { "0" { "executable" "FFXiiiLauncher.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "228990" { "config" { "oslist" "windows" } "depotfromapp" "228980" "sharedinstall" "1" }
    "292121" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "1" "size" "30738676601" "download" "56430184027278" } } }
    "292122" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "2" "size" "61851360699" "download" "103960316066861" } } }
    "292123" { "manifests" { "public" { "gid" "3" "size" "1000" "download" "400" } } }
  }
})";
  const auto details = DetailsFromAppInfo(292120, buffer);
  assert(details.available && details.windows_installable);
  assert(details.install_bytes == 30738676601ull + 61851360699ull + 1000u);
  assert(details.download_bytes == 30738676601ull + 61851360699ull + 400u);
}

}  // namespace

int main() {
  RequestsAreDeduplicatedPerGeneration();
  NewGenerationDropsResultsAndRefetches();
  FailuresAreRetryable();
  AppInfoBecomesWindowsEstimate();
  DefaultBetaKeyIsThePublicBranch();
  ImpossibleDownloadSizeFallsBackToInstallSize();
}
