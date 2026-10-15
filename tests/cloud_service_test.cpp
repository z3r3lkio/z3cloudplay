#include "steam/cloud_service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>
#include <vector>
#include <mutex>
#include <thread>

using namespace z3cloudplay;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

class Sessions final : public SessionRepository {
 public:
  bool present = true;
  Result<std::optional<StoredSession>> Load() const override {
    if (!present) return Result<std::optional<StoredSession>>::Success(std::nullopt);
    return Result<std::optional<StoredSession>>::Success(StoredSession{123, "synthetic", "refresh", "access", {}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};

class Runner final : public CloudRunner {
 public:
  std::mutex mutex;
  CloudState next_check = CloudState::kSyncing;
  int checks = 0, applies = 0;
  CloudResolution last_resolution = CloudResolution::kAutomatic;
  bool last_confirm = false;
  std::string last_root;
  std::string last_prefix;
  std::map<GameId, CloudState> states;  // per game; next_check otherwise
  std::vector<CloudDrift> drift;
  std::vector<GameId> checked;
  int check_delay_ms = 0;
  int removes = 0;
  std::function<void(std::uint64_t, std::uint64_t)> progress;
  CloudReport Check(const StoredSession&, const CloudLocation& location, const std::atomic<bool>&) override {
    if (check_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(check_delay_ms));
    std::lock_guard<std::mutex> lock(mutex);
    ++checks;
    checked.push_back(location.id);
    if (const auto found = states.find(location.id); found != states.end()) {
      CloudReport report;
      report.state = found->second;
      report.message = "checked";
      report.first_sync = found->second == CloudState::kConflict;
      return report;
    }
    last_root = location.game_root;
    last_prefix = location.prefix_root;
    CloudReport report;
    report.state = next_check;
    report.message = "checked";
    report.downloads = 2;
    return report;
  }
  CloudReport Apply(const StoredSession&, const CloudLocation&, CloudResolution resolution, bool confirm,
                    const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++applies;
    if (progress) progress(50, 100);
    last_resolution = resolution;
    last_confirm = confirm;
    CloudReport report;
    report.state = CloudState::kSynced;
    report.message = "2 saves downloaded";
    return report;
  }
  std::vector<CloudDrift> QuickCheck(const StoredSession&, const std::vector<CloudLocation>& locations,
                                     const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto out = drift;
    out.resize(locations.size(), CloudDrift::kUnknown);
    return out;
  }
  bool RemoveLocal(const CloudLocation&, std::string* message) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++removes;
    *message = "1 save deleted";
    return true;
  }
  void SetProgress(std::function<void(std::uint64_t, std::uint64_t)> callback) override { progress = std::move(callback); }
};

template <typename Ready>
void AwaitAt(int line, CloudService& service, Ready ready) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready(service.Snapshot())) return;
    std::this_thread::sleep_for(1ms);
  }
  std::fprintf(stderr, "deadline waiting at line %d\n", line);
  std::exit(1);
}

const CloudReport* Of(const CloudSnapshot& snapshot, GameId id) {
  for (const auto& game : snapshot.games) if (game.id == id) return &game.report;
  return nullptr;
}

void Jobs() {
  const auto base = std::filesystem::temp_directory_path() / ("z3cloudplay-cloud-jobs-" + std::to_string(getpid()));
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  const auto at = [&](GameId id, bool check_only = false) {
    CloudLocation location{id, "/games/" + std::to_string(id), (base / std::to_string(id)).string(), "/bottle"};
    location.check_only = check_only;
    return location;
  };
  // A result kept from an earlier run comes back at start.
  std::filesystem::create_directories(base / "7");
  std::ofstream(base / "7/cloud-status.txt") << "version=1\nstate=conflict\nmessage=Both changed\nfirst_sync=1\nlast_sync=0\n"
                                                "checked=1700000000\nlocal_files=2\nremote_files=3\n";
  Sessions sessions;
  Runner runner;
  CloudService service(runner, sessions);
  service.SetClock([] { return std::int64_t{1'800'000'000}; });
  service.Restore(base.string());
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) {
    auto* r = Of(s, 7);
    return r && r->state == CloudState::kConflict && r->first_sync && r->remote_files == 3 && r->checked == 1'700'000'000;
  });

  // Settle three games: 1 and 2 in step, 3 unknown to Steam Cloud.
  runner.states = {{1, CloudState::kUpToDate}, {2, CloudState::kUpToDate}, {3, CloudState::kUnsupported}};
  for (GameId id : {1, 2, 3}) {
    service.Sync(at(id));
    AwaitAt(__LINE__, service, [id](const CloudSnapshot& s) { auto* r = Of(s, id); return r && r->checked == 1'800'000'000; });
  }
  CHECK(Of(service.Snapshot(), 1)->last_sync == 1'800'000'000 && Of(service.Snapshot(), 3)->last_sync == 0);
  CHECK(std::filesystem::exists(base / "1/cloud-status.txt"));

  // The quick check fully checks only what drifted or was never settled: 2 changed locally, 4 is new,
  // 7 is a settled conflict that did not change, 3 has no Steam Cloud. 2's transfers run at once.
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.checked.clear();
    runner.drift = {CloudDrift::kUnchanged, CloudDrift::kLocal, CloudDrift::kUnknown, CloudDrift::kUnknown, CloudDrift::kUnchanged};
    runner.states[2] = CloudState::kSyncing;
    runner.states.erase(4);
  }
  runner.next_check = CloudState::kUpToDate;
  service.StartJob(CloudJobKind::kQuickCheck, {at(1), at(2), at(3), at(4), at(7)});
  CHECK(service.Snapshot().job.kind == CloudJobKind::kQuickCheck && service.Snapshot().job.total == 5);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return s.job.kind == CloudJobKind::kNone; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK((runner.checked == std::vector<GameId>{2, 4}));
    CHECK(runner.applies == 1 && runner.last_resolution == CloudResolution::kAutomatic);
  }
  auto snapshot = service.Snapshot();
  CHECK(Of(snapshot, 2)->state == CloudState::kSynced && Of(snapshot, 7)->state == CloudState::kConflict);
  for (const auto& game : snapshot.games) CHECK(!game.report.queued);

  // Sync all checks every game but settles no choice; a kept bottle's transfers wait for the player.
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.checked.clear();
    runner.states = {{1, CloudState::kSyncing}, {7, CloudState::kConflict}, {9, CloudState::kSyncing}};
  }
  service.StartJob(CloudJobKind::kSyncAll, {at(1), at(7), at(9, true)});
  service.StartJob(CloudJobKind::kQuickCheck, {at(1)});  // ignored while a job runs
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return s.job.kind == CloudJobKind::kNone; });
  snapshot = service.Snapshot();
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK((runner.checked == std::vector<GameId>{1, 7, 9}) && runner.applies == 2);
  }
  CHECK(Of(snapshot, 1)->state == CloudState::kSynced && Of(snapshot, 7)->state == CloudState::kConflict);
  CHECK(Of(snapshot, 9)->state == CloudState::kWaiting);

  // Stop ends the job after the game it is on.
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.checked.clear();
    runner.states = {};
  }
  runner.next_check = CloudState::kUpToDate;
  runner.check_delay_ms = 40;
  service.StartJob(CloudJobKind::kSyncAll, {at(1), at(2), at(3), at(4)});
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return s.job.current == 1; });
  service.StopJob();
  CHECK(service.Snapshot().job.stopping);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return s.job.kind == CloudJobKind::kNone; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK(runner.checked.size() == 1);
  }
  runner.check_delay_ms = 0;

  // Two failed checks in a row end a job: Steam is unreachable.
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.checked.clear();
    runner.states = {{1, CloudState::kFailed}, {2, CloudState::kFailed}};
  }
  service.StartJob(CloudJobKind::kSyncAll, {at(1), at(2), at(3)});
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return s.job.kind == CloudJobKind::kNone; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK((runner.checked == std::vector<GameId>{1, 2}));
  }
  CHECK(!Of(service.Snapshot(), 3)->queued);
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.states.clear();
  }

  // A restored choice is checked again, then applied.
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.checked.clear();
    runner.states = {{7, CloudState::kConflict}};
  }
  const int applies_before = runner.applies;
  CHECK(service.Resolve(at(7), CloudResolution::kUseCloud, true) > 0);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { auto* r = Of(s, 7); return r && r->state == CloudState::kSynced; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK((runner.checked == std::vector<GameId>{7}) && runner.applies == applies_before + 1 &&
          runner.last_resolution == CloudResolution::kUseCloud);
    runner.states.clear();
  }

  // The gate is asked as each game comes up: a game started meanwhile is skipped and handed back.
  service.SetGate([](GameId id) { return id != 2; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.checked.clear();
  }
  service.StartJob(CloudJobKind::kSyncAll, {at(1), at(2), at(3)});
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return s.job.kind == CloudJobKind::kNone; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK((runner.checked == std::vector<GameId>{1, 3}));
  }
  CHECK((service.TakeDeferred() == std::vector<GameId>{2}) && service.TakeDeferred().empty());
  CHECK(Of(service.Snapshot(), 2)->state != CloudState::kChecking);
  service.SetGate({});

  // Deleting local saves never downloads them straight back.
  runner.next_check = CloudState::kSyncing;
  service.RemoveLocal(at(1));
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) {
    auto* r = Of(s, 1);
    return r && r->state == CloudState::kWaiting && r->message == "1 save deleted";
  });
  // Forgetting removes the state folder and the result.
  service.Forget(at(1));
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { return !Of(s, 1); });
  CHECK(!std::filesystem::exists(base / "1"));
  service.Shutdown();
  std::filesystem::remove_all(base);
}

}  // namespace

int main() {
  Jobs();
  Sessions sessions;
  Runner runner;
  CloudService service(runner, sessions);
  const CloudLocation game{42, "/games/42", "/games/.z3cloudplay-saves/42",
                           "/data/z3cloudplay/games/install-42/library/prefixes/game"};

  // Transfers that need no choice run straight after the check.
  service.Sync(game);
  CHECK(Of(service.Snapshot(), 42) && (Of(service.Snapshot(), 42)->state == CloudState::kChecking ||
                                        Of(service.Snapshot(), 42)->state == CloudState::kSyncing ||
                                        Of(service.Snapshot(), 42)->state == CloudState::kSynced));
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { auto* r = Of(s, 42); return r && r->state == CloudState::kSynced; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK(runner.checks == 1 && runner.applies == 1 && runner.last_resolution == CloudResolution::kAutomatic);
    CHECK(runner.last_root == "/games/42");
    CHECK(runner.last_prefix == game.prefix_root);
  }

  // A conflict waits for the player, then applies their choice.
  runner.next_check = CloudState::kConflict;
  service.Sync(game);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { auto* r = Of(s, 42); return r && r->state == CloudState::kConflict; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK(runner.applies == 1);
  }
  service.Resolve(game, CloudResolution::kUseCloud, true);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { auto* r = Of(s, 42); return r && r->state == CloudState::kSynced; });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK(runner.applies == 2 && runner.last_resolution == CloudResolution::kUseCloud && runner.last_confirm);
  }

  // Resolve does nothing unless a choice is pending.
  service.Resolve(game, CloudResolution::kUseLocal, false);
  std::this_thread::sleep_for(20ms);
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    CHECK(runner.applies == 2);
  }

  // Up to date: no transfer.
  runner.next_check = CloudState::kUpToDate;
  service.Sync(game);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) { auto* r = Of(s, 42); return r && r->state == CloudState::kUpToDate; });

  // Without a session the sync fails visibly.
  sessions.present = false;
  service.Sync(game);
  AwaitAt(__LINE__, service, [](const CloudSnapshot& s) {
    auto* r = Of(s, 42);
    return r && r->state == CloudState::kFailed && r->message == "Sign in to Steam to sync saves.";
  });
  service.Shutdown();
  std::puts("cloud service tests passed");
}
