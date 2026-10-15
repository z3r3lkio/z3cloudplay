#include "homebrew/runtime_host.hpp"
#include "platform/managed_directory.h"
#include "platform/storage.h"
#include "runtime/artifact_hash.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <memory>
#include <unistd.h>

using namespace z3cloudplay::homebrew;
using namespace z3cloudplay::runtime;
using namespace z3cloudplay::ps5;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
namespace {
std::string g_runtime_dir;  // the repository's bundled runtime data (recipes, presets, input)

void Write(const fs::path& path, const std::string& bytes, unsigned mode) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
  CHECK(chmod(path.c_str(), mode) == 0);
}
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

struct Fixture {
  fs::path root;
  unsigned database_reads = 0, registry_mutations = 0;
  bool registered = false;
  // ShadowMountPlus postpones scans while a title runs: with registers false a
  // submission is accepted and listed, but not installed until registered is set.
  bool registers = true, submitted = false;
  std::shared_ptr<std::atomic<int>> activity = std::make_shared<std::atomic<int>>(static_cast<int>(NativeActivity::kStopped));
  Fixture() {
    char name[] = "/tmp/z3cloudplay-runtime-host-XXXXXX"; CHECK(mkdtemp(name));
    root = fs::canonical(name);
    fs::create_directories(root / "data/homebrew");
  }
  ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
  RuntimeHostConfig Config() {
    RuntimeHostConfig config;
    config.boundary = root / "data";
    config.state_root = root / "data/runtime-state";
    config.homebrew_root = root / "data/homebrew";
    config.native.database = [this](const std::string& title) {
      ++database_reads;
      if (!registered) return TitleDatabaseObservation{TitleAvailability::kFree, {}};
      return TitleDatabaseObservation{TitleAvailability::kOccupied,
          InstalledTitleIdentity{title, "UP9000-FAKE78001_00-PROSPEROWIN00001", "/user/app/FAKE78001/sce_sys"}};
    };
    config.native.activity = [activity = activity](const std::string&) { return static_cast<NativeActivity>(activity->load()); };
    // ShadowMount loopback API: registration makes the title appear installed.
    config.registry_transport = [this](const std::string& route, const std::string&) -> std::optional<std::string> {
      if (route != "/api/v1/games") { ++registry_mutations; submitted = true; registered = registers; return R"({"status":0})"; }
      if (!registered && submitted)
        return R"({"status":0,"games":[{"title_id":"FAKE78001","installed":false,"managed":true,)"
               R"("source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"}]})";
      if (!registered) return R"({"status":0,"games":[]})";
      return R"({"status":0,"games":[{"title_id":"FAKE78001","installed":true,"managed":true,)"
             R"("source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"}]})";
    };
    config.admit_kit = [](std::string* reason) -> std::optional<RuntimeKit> {
      *reason = "No verified compatibility kit is installed.";
      return std::nullopt;
    };
    config.runtime_dir = g_runtime_dir;
    return config;
  }
};

RuntimeSnapshot WaitFor(const RuntimeHost& host, const std::function<bool(const RuntimeSnapshot&)>& done,
                        int seconds = 10) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  for (;;) {
    auto snapshot = host.Snapshot();
    if (done(snapshot)) return snapshot;
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
bool Started(const RuntimeSnapshot& s) { return s.status != RuntimeHostStatus::kStarting; }

RuntimePrepareRequest Hl2() { return {220, "/data/homebrew/FAKE77031/games/220", "internal", "Half-Life \"2\""}; }

RuntimeInstall Record() {
  RuntimeInstall record;
  record.install_id = "steam-220"; record.app_id = 220; record.default_profile_id = "z3cloudplay-steam-220";
  record.runtime_bundle_id = "wine-wow64"; record.recipe_id = "steam-220-hl2-v1"; record.recipe_version = "1";
  record.launch_entry_id = "hl2"; record.launch_path = "hl2.exe";
  record.install_path = "/data/homebrew/FAKE77031/games/220"; record.volume_id = "internal";
  return record;
}

// A synthetic admitted kit with the production file layout, real SHA-256 and
// the repository's HL2 recipe. It is not a runnable Wine runtime.
RuntimeKit SyntheticKit(const fs::path& kit, const std::string& eboot = "native",
                        const std::string& kit_id = "kit-1", char files_digest = 'a',
                        const std::string& runtime = "runtime", const std::string& revision = "rev") {
  Write(kit / "titles/variant/eboot.bin", eboot, 0755);
  Write(kit / "titles/variant/sce_sys/param.json",
        "{\n  \"contentId\": \"UP9000-FAKE78001_00-PROSPEROWIN00001\",\n  \"localizedParameters\": {\n"
        "    \"en-US\": {\n      \"titleName\": \"Prospero Win\"\n    }\n  },\n  \"titleId\": \"FAKE78001\"\n}\n", 0644);
  Write(kit / "titles/variant/sce_sys/icon0.png", "kit-icon", 0644);
  Write(kit / ("runtime/wine/" + revision + "/lib/core.prx"), runtime, 0755);
  Write(kit / "template/bottle/system.reg", "WINE REGISTRY Version 2\n", 0644);
  Write(kit / "template/bottle/user.reg", "WINE REGISTRY Version 2\n", 0644);
  Write(kit / "template/bottle/drive_c/windows/win.ini", "", 0644);
  Write(kit / "template/bottle/dosdevices/.pw-symlinks", "c:\t../drive_c\nz:\t/\n", 0644);
  Write(kit / "recipes/hl2.json", Read(fs::path(g_runtime_dir) / "recipes/steam-220-hl2-v1.json"), 0644);
  RuntimeKit admitted;
  auto& view = admitted.view;
  view.root = kit; view.kit_id = kit_id; view.files_sha256 = std::string(64, files_digest);
  view.runtime = {"wine-wow64", revision, "runtime/wine/" + revision};
  view.template_path = "template/bottle";
  view.template_drive_table = "template/bottle/dosdevices/.pw-symlinks";
  view.recipe = {"steam-220-hl2-v1", "1", "recipes/hl2.json", *HashArtifactFile(kit / "recipes/hl2.json")};
  view.variants.push_back({"variant", "FAKE78001", "titles/variant"});
  for (const auto& [path, mode] : std::vector<std::pair<std::string, unsigned>>{
           {"titles/variant/eboot.bin", 0755}, {"titles/variant/sce_sys/param.json", 0644},
           {"titles/variant/sce_sys/icon0.png", 0644},
           {"runtime/wine/" + revision + "/lib/core.prx", 0755}, {"template/bottle/system.reg", 0644},
           {"template/bottle/user.reg", 0644}, {"template/bottle/drive_c/windows/win.ini", 0644},
           {"template/bottle/dosdevices/.pw-symlinks", 0644}, {"recipes/hl2.json", 0644}})
    view.files.push_back({path, fs::file_size(kit / path), *HashArtifactFile(kit / path), mode});
  view.artifacts = {
      {"runtime", revision, *HashArtifactFile(kit / ("runtime/wine/" + revision + "/lib/core.prx")),
       ImmutableArtifactLocation::kStagedTitle, "win/wine/lib/core.prx"},
      {"recipe", "steam-220-hl2-v1-1", *HashArtifactFile(kit / "recipes/hl2.json"),
       ImmutableArtifactLocation::kKit, "recipes/hl2.json"}};
  admitted.capabilities = {"dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
  return admitted;
}
// The same kit with one shell for every game: its own param.json names the template title.
RuntimeKit AnyTitleKit(const fs::path& kit) {
  auto admitted = SyntheticKit(kit);
  const auto param = kit / "titles/variant/sce_sys/param.json";
  Write(param,
        "{\n  \"conceptId\": \"78000\",\n  \"contentId\": \"UP9000-FAKE78000_00-PROSPEROWIN00001\",\n"
        "  \"localizedParameters\": {\n    \"en-US\": {\n      \"titleName\": \"Prospero Win\"\n    }\n  },\n"
        "  \"titleId\": \"FAKE78000\"\n}\n", 0644);
  for (auto& file : admitted.view.files)
    if (file.path == "titles/variant/sce_sys/param.json") {
      file.size = fs::file_size(param);
      file.sha256 = *HashArtifactFile(param);
    }
  admitted.view.variants = {{"any-title", "FAKE78000", "titles/variant", true}};
  admitted.capabilities.push_back("title-any-id");
  return admitted;
}
}  // namespace

// One shell for every game: the game gets a new title of its own (the lowest free FAKE78xxx),
// stamped with its own title ID, and its helper's allow-list names it before the title exists.
// Without a way to write that list nothing is prepared.
void AnyTitleKitMakesTheGameItsOwnTitle() {
  for (const int scenario : {0, 1, 2}) {
    const bool writer = scenario != 0;
    Fixture fixture;
    auto config = fixture.Config();
    config.host_namespace_prefix = fixture.root;
    config.package_paths.console_data_root = "/data";
    config.package_paths.host_data_root = fixture.root / "data";
    config.package_operations.hash_file = HashArtifactFile;
    config.package_operations.hash_bytes = HashArtifactBytes;
    config.package_operations.free_space_unverifiable = true;
    config.package_operations.validate_bottle = [](const fs::path& bottle) {
      return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
    };
    const auto kit = AnyTitleKit(fixture.root / "kit");
    config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    unsigned uuids = 0;
    config.uuid = [&uuids]() -> std::optional<std::string> {
      ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
    };
    config.unix_time = [] { return std::int64_t{1760000000}; };
    std::vector<std::string> lists;
    if (writer)
      config.write_title_allow_list = [&lists, scenario](const std::string& contents, std::string*) {
        lists.push_back(contents);
        return scenario != 2 || lists.size() != 2;
      };
    Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (scenario == 2) {
      CHECK(done.refusals.count(220));
      host.Prepare(Hl2());
      done = WaitFor(host, [](const RuntimeSnapshot& s) {
        return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
      }, 60);
    }
    if (!writer) {
      CHECK(done.refusals.count(220) && done.refusals.at(220) == "This console can't let new PS5 titles start.");
      CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001"));
      host.Shutdown();
      continue;
    }
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs.size() == 1);
    CHECK(done.installs[0].state == PreparationState::kReady && done.installs[0].title_id == "FAKE78001");
    // Startup wrote the (empty) list; the reservation listed the new title before staging.
    CHECK(lists.size() == (scenario == 2 ? 3u : 2u) && lists[0].empty() && lists.back() == "FAKE78001\n");
    const auto param = Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/param.json");
    CHECK(param.find(R"("titleId": "FAKE78001")") != std::string::npos);
    CHECK(param.find(R"("conceptId": "78001")") != std::string::npos);
    CHECK(param.find(R"("contentId": "UP9000-FAKE78001_00-PROSPEROWIN00001")") != std::string::npos);
    CHECK(param.find("FAKE78000") == std::string::npos && param.find(R"("titleName": "Half-Life \"2\"")") != std::string::npos);
    CHECK(Read(fixture.root / "data/homebrew/FAKE78001/eboot.bin") == "native");
    CHECK(done.installs[0].title_current && done.installs[0].admission == "ok");
    host.Shutdown();
    std::atomic<bool> list_writable{false};
    std::atomic<unsigned> retries{0};
    config.write_title_allow_list = [&](const std::string&, std::string*) { ++retries; return list_writable.load(); };
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    WaitFor(restarted, Started);
    restarted.Prepare(Hl2());
    const auto refused = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.refusals.count(220); });
    CHECK(retries >= 2);
    CHECK(refused.refusals.at(220) != "This game is already prepared.");
    list_writable = true;
    restarted.Prepare(Hl2());
    WaitFor(restarted, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && s.refusals.count(220) && s.refusals.at(220) == "This game is already prepared.";
    });
    restarted.Shutdown();
  }
}

// Without an admitted kit, the composed services start, report why preparation
// is unavailable and refuse Prepare without creating durable records.
void NoKitRefusesPreparationWithoutDurableEffects() {
  Fixture fixture;
  RuntimeHost host(fixture.Config());
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kReady);
  CHECK(!started.kit_available && started.kit_reason == "No verified compatibility kit is installed.");
  CHECK(started.installs.empty());
  struct stat info{};
  CHECK(stat((fixture.root / "data/runtime-state").c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
  host.Prepare(Hl2());
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.refusals.at(220) == "No verified compatibility kit is installed.");
  host.Shutdown();
  CHECK(host.Snapshot().status == RuntimeHostStatus::kStopped);
  CHECK(!fs::exists(fixture.root / "data/runtime-state/installs.json"));
  CHECK(fixture.registry_mutations == 0);
  CHECK(fs::is_empty(fixture.root / "data/homebrew"));
}

// A preparation asked for before the runtime is ready is published as queued until the worker
// takes it, so Play and Recreate bottle wait instead of reporting it lost.
void QueuedPreparationIsPublishedUntilTaken() {
  Fixture fixture;
  RuntimeHost host(fixture.Config());
  host.Prepare(Hl2());
  CHECK(host.Snapshot().queued_prepares.count(220) == 1);
  CHECK(host.Start());
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.queued_prepares.empty() && !refused.preparing_app);
  host.Shutdown();
}

// Startup reads durable records only. A journaled record is reported as needing
// recovery; it is not resumed, re-registered or rewritten.
void StartupReportsJournalsWithoutResuming() {
  Fixture fixture;
  auto config = fixture.Config();
  fs::create_directories(config.state_root); CHECK(chmod(config.state_root.c_str(), 0700) == 0);
  {
    z3cloudplay::Storage storage(config.state_root.string());
    RuntimeInstallRepository repository(storage, "installs.json", [](const std::string&) {
      return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
    });
    CHECK(repository.Load().ok());
    const auto record = Record();
    ReserveRequest request;
    request.install_id = record.install_id; request.app_id = record.app_id;
    request.default_profile_id = record.default_profile_id; request.runtime_bundle_id = record.runtime_bundle_id;
    request.recipe_id = record.recipe_id; request.recipe_version = record.recipe_version;
    request.launch_entry_id = record.launch_entry_id; request.launch_path = record.launch_path;
    request.install_path = record.install_path; request.volume_id = record.volume_id;
    request.kit_title_variants = {"FAKE78001"};
    auto reserved = repository.Reserve(request); CHECK(reserved.ok());
    auto journaled = reserved.value();
    journaled.state = PreparationState::kRegistering;
    journaled.journal = PreparationJournal{"11111111-2222-4333-8444-555555555555", "settings-publication", "register"};
    CHECK(repository.Update(journaled).ok());
  }
  const auto before = Read(config.state_root / "installs.json");
  CHECK(!before.empty());
  const auto database_reads_before = fixture.database_reads;
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kReady && started.installs.size() == 1);
  const auto& view = started.installs[0];
  CHECK(view.app_id == 220 && view.title_id == "FAKE78001" && view.state == PreparationState::kRegistering);
  CHECK(view.recovery_pending);
  host.Shutdown();
  CHECK(Read(config.state_root / "installs.json") == before);
  CHECK(fixture.registry_mutations == 0 && fixture.database_reads == database_reads_before);
}

// A public or foreign state root is refused rather than taken over.
void PublicStateRootIsUnavailable() {
  Fixture fixture;
  auto config = fixture.Config();
  fs::create_directories(config.state_root); CHECK(chmod(config.state_root.c_str(), 0755) == 0);
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kUnavailable && !started.reason.empty());
  host.Prepare(Hl2());
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.refusals.at(220) == started.reason);
  host.Shutdown();
  struct stat info{};
  CHECK(stat(config.state_root.c_str(), &info) == 0 && (info.st_mode & 0777) == 0755);
}

// ShadowMountPlus registers a submitted title only once no title runs (Z3CloudPlay
// included): preparation ends as soon as the registration is submitted, with the
// game waiting for the console and no refusal. A later check only asks the console:
// once the title is registered it becomes Ready, never submitted again.
void PreparationEndsAwaitingRegistration() {
  Fixture fixture;
  fixture.registers = false;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  const auto began = std::chrono::steady_clock::now();
  host.Prepare(Hl2());
  const auto waiting = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.refusals.count(220) || s.installs[0].awaiting_registration);
  }, 60);
  if (waiting.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", waiting.refusals.at(220).c_str());
  CHECK(!waiting.refusals.count(220) && waiting.installs[0].awaiting_registration);
  CHECK(waiting.installs[0].state != PreparationState::kReady && waiting.installs[0].recovery_pending);
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(20));  // no polling for the console
  const auto submissions = fixture.registry_mutations;
  CHECK(submissions > 0 && fixture.submitted && !fixture.registered);

  // Asked again while the console still has not registered it: still waiting, nothing resubmitted.
  host.Prepare(Hl2());
  auto again = WaitFor(host, [](const RuntimeSnapshot& s) { return s.preparing_app.has_value(); });
  again = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app; }, 30);
  CHECK(!again.refusals.count(220) && again.installs[0].awaiting_registration);
  CHECK(fixture.registry_mutations == submissions);

  // The player left Z3CloudPlay and ShadowMountPlus registered it: the check finds it Ready.
  fixture.registered = true;
  host.Prepare(Hl2());
  const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 30);
  CHECK(!ready.refusals.count(220) && ready.installs[0].state == PreparationState::kReady);
  CHECK(!ready.installs[0].awaiting_registration && !ready.installs[0].recovery_pending);
  CHECK(fixture.registry_mutations == submissions);
  host.Shutdown();
}

// An existing download is prepared in place: the host stages its own title,
// copies a private bottle, publishes settings through OWNER, registers once and
// commits Ready. Game files, DLLs and saves are preserved byte for byte.
void PreparesExistingDownloadToReady() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.title_icon = [](std::uint32_t app) -> std::optional<std::string> {
    return app == 220 ? std::optional<std::string>("game-icon-png") : std::nullopt;
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  Write(game / "bin/steam_api.dll", "unchanged-original-dll", 0644);
  Write(game / "hl2/save/quick.sav", "existing-player-save", 0644);
  CHECK(chmod(game.c_str(), 0777) == 0);  // console downloads are world-writable

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220));
  CHECK(done.installs.size() == 1);
  const auto& ready = done.installs[0];
  CHECK(ready.state == PreparationState::kReady && ready.title_id == "FAKE78001" && !ready.recovery_pending);
  CHECK(fixture.registry_mutations == 2 && uuids == 1);
  // Game files stay at the recorded download root and are untouched.
  CHECK(Read(game / "hl2.exe") == "existing-game");
  CHECK(Read(game / "bin/steam_api.dll") == "unchanged-original-dll");
  CHECK(Read(game / "hl2/save/quick.sav") == "existing-player-save");
  CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001/games"));
  struct stat game_info{};
  CHECK(stat(game.c_str(), &game_info) == 0 && (game_info.st_mode & 07777) == 0755);
  // The staged title is readable by ShadowMount but not writable by others.
  struct stat title_info{};
  for (const auto* relative : {"data/homebrew/FAKE78001", "data/homebrew/FAKE78001/sce_sys"})
    CHECK(stat((fixture.root / relative).c_str(), &title_info) == 0 && (title_info.st_mode & 07777) == 0755);
  // The PS5 title shows the game's own name and icon, not the kit's.
  const auto param = Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/param.json");
  CHECK(param.find(R"("titleName": "Half-Life \"2\"")") != std::string::npos);
  CHECK(param.find("Prospero Win") == std::string::npos && param.find("FAKE78001") != std::string::npos);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/icon0.png") == "game-icon-png");
  CHECK(stat((fixture.root / "data/homebrew/FAKE78001/sce_sys/icon0.png").c_str(), &title_info) == 0 &&
        (title_info.st_mode & 07777) == 0644);
  CHECK(done.installs[0].title_current);
  // The descriptor binds the existing root; the bottle is private.
  const auto descriptor = Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json");
  CHECK(descriptor.find(R"("schema_version":2)") != std::string::npos);
  CHECK(descriptor.find(R"("game_root":"/data/homebrew/FAKE77031/games/220")") != std::string::npos);
  struct stat info{};
  CHECK(stat((fixture.root / "data/z3cloudplay/games/steam-220/library/prefixes/game").c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0700);
  // The finished publication's request is no longer named by any journal: an
  // explicit cleanup removes it.
  const auto request = fixture.root / "data/runtime-state/requests/settings-request-00000000-0000-4000-8000-000000000001.v1";
  CHECK(fs::is_regular_file(request));
  CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_requests == 1; }).leftover_requests == 1);
  host.CleanUpRequests();
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_requests == 0; });
  CHECK(!fs::exists(request));
  // A second request does not redo anything.
  host.Prepare(Hl2());
  const auto again = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(again.refusals.at(220) == "This game is already prepared.");
  CHECK(fixture.registry_mutations == 2);
  host.Shutdown();
}

// A capture refused after control setup (the console's world-writable game
// folder) leaves a settings-control journal. An explicit Prepare resumes the
// saved request and reaches Ready with one registration.
void ResumesInterruptedCaptureFromSavedRequest(bool tamper_backup = false) {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  unsigned uuids = 0;
  // Called just before capture: the first attempt sees a world-writable folder.
  config.uuid = [&uuids, game]() -> std::optional<std::string> {
    if (++uuids == 1) CHECK(chmod(game.c_str(), 0777) == 0);
    return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(game / "hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto failed = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); }, 60);
  CHECK(failed.installs.size() == 1 && failed.installs[0].state != PreparationState::kReady);
  CHECK(failed.installs[0].recovery_pending);
  // The saved request the journal names is in use: never leftover, never removed.
  CHECK(failed.leftover_requests == 0);
  host.CleanUpRequests();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(fs::is_regular_file(fixture.root / "data/runtime-state/requests/settings-request-00000000-0000-4000-8000-000000000001.v1"));
  const auto backup = fixture.root / "data/z3cloudplay/games/steam-220/.z3cloudplay-control" /
                      "registry-backup-00000000-0000-4000-8000-000000000001.v1";
  CHECK(fs::is_regular_file(backup));
  if (tamper_backup) {  // Same size, different bytes: not this capture's backup.
    auto bytes = Read(backup); bytes[0] = bytes[0] == 'X' ? 'Y' : 'X';
    Write(backup, bytes, 0600);
  }
  const auto before = host.Snapshot().revision;
  host.Prepare(Hl2());
  WaitFor(host, [before](const RuntimeSnapshot& s) { return s.revision > before && !s.refusals.count(220); });
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() &&
        (s.installs[0].state == PreparationState::kReady || s.refusals.count(220));
  }, 60);
  if (tamper_backup) {
    CHECK(done.refusals.count(220) && done.refusals.at(220).find("orphan") != std::string::npos);
    CHECK(done.installs[0].state != PreparationState::kReady && fixture.registry_mutations == 0);
    host.Shutdown();
    return;
  }
  if (done.refusals.count(220)) std::fprintf(stderr, "Resume refused: %s\n", done.refusals.at(220).c_str());
  CHECK(done.installs[0].state == PreparationState::kReady && !done.installs[0].recovery_pending);
  CHECK(fixture.registry_mutations == 2 && uuids == 1);
  host.Shutdown();
}

// A respun kit with the same id, runtime and recipe (for example a new wrapper
// build) updates the installed title in place: changed files and owner markers
// only, no new registration, publication or bottle changes. A different kit id
// is refused.
void RefreshesInstalledTitleToRespunKit() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  config.uuid = []() -> std::optional<std::string> { return std::string("00000000-0000-4000-8000-000000000001"); };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  {
    const auto kit = SyntheticKit(fixture.root / "kit");
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    RuntimeHost host(first);
    CHECK(host.Start());
    host.Prepare(Hl2());
    const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    CHECK(ready.installs[0].title_current);
    host.Shutdown();
  }
  const auto bottle_reg = fixture.root / "data/z3cloudplay/games/steam-220/library/prefixes/game/user.reg";
  const auto registry_before = Read(bottle_reg);
  const auto installs_before = Read(fixture.root / "data/runtime-state/installs.json");
  CHECK(fixture.registry_mutations == 2);
  {  // A different kit id is not a respin; it needs republication.
    const auto other = SyntheticKit(fixture.root / "kit-other", "native", "kit-2", 'b');
    auto second = config;
    second.admit_kit = [other](std::string*) { return std::optional<RuntimeKit>(other); };
    RuntimeHost host(second);
    CHECK(host.Start());
    CHECK(!WaitFor(host, Started).installs[0].title_current);
    host.Prepare(Hl2());
    const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); });
    CHECK(refused.refusals.at(220).find("compatible kit") != std::string::npos);
    host.Shutdown();
  }
  const auto respun = SyntheticKit(fixture.root / "kit-respun", "native-2", "kit-1", 'c');
  auto third = config;
  third.admit_kit = [respun](std::string*) { return std::optional<RuntimeKit>(respun); };
  RuntimeHost host(third);
  CHECK(host.Start());
  CHECK(!WaitFor(host, Started).installs[0].title_current);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Refresh refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/eboot.bin") == "native-2");
  CHECK(Read(bottle_reg) == registry_before);
  CHECK(Read(fixture.root / "data/runtime-state/installs.json") == installs_before);
  CHECK(fixture.registry_mutations == 2);
  host.Shutdown();

  // A rebuilt runtime (same artifacts, new bytes) upgrades the title in place:
  // the new runtime reaches the title, the record keeps the new hashes, the
  // registration and bottle registry stay.
  const auto rebuilt = SyntheticKit(fixture.root / "kit-rebuilt", "native-2", "kit-1", 'd', "runtime-neutral", "rev-neutral");
  auto fourth = config;
  fourth.admit_kit = [rebuilt](std::string*) { return std::optional<RuntimeKit>(rebuilt); };
  RuntimeHost upgraded(fourth);
  CHECK(upgraded.Start());
  CHECK(!WaitFor(upgraded, Started).installs[0].title_current);
  upgraded.Prepare(Hl2());
  const auto up = WaitFor(upgraded, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
  }, 60);
  if (up.refusals.count(220)) std::fprintf(stderr, "Upgrade refused: %s\n", up.refusals.at(220).c_str());
  CHECK(!up.refusals.count(220) && up.installs[0].state == PreparationState::kReady);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/win/wine/lib/core.prx") == "runtime-neutral");
  const auto upgraded_installs = Read(fixture.root / "data/runtime-state/installs.json");
  CHECK(upgraded_installs != installs_before &&
        upgraded_installs.find(*HashArtifactBytes("runtime-neutral")) != std::string::npos);
  CHECK(Read(bottle_reg) == registry_before);
  CHECK(fixture.registry_mutations == 2);
  upgraded.Shutdown();
  // A restart keeps the upgraded title current.
  RuntimeHost again(fourth);
  CHECK(again.Start());
  CHECK(WaitFor(again, Started).installs[0].title_current);
  again.Shutdown();
}

// A game run that leaves the bottle registry wider than admission accepts
// (Wine saving with umask 0) is reported at startup, and Prepare repairs it.
void RepairsBottleRegistryModes() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = []() -> std::optional<std::string> { return std::string("00000000-0000-4000-8000-000000000001"); };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    host.Prepare(Hl2());
    WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    host.Shutdown();
  }
  const auto user_reg = fixture.root / "data/z3cloudplay/games/steam-220/library/prefixes/game/user.reg";
  CHECK(chmod(user_reg.c_str(), 0666) == 0);
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs[0].admission.find("stage 7") != std::string::npos);
  host.Prepare(Hl2());
  const auto repaired = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].admission == "ok";
  }, 60);
  CHECK(!repaired.refusals.count(220));
  struct stat info{};
  CHECK(stat(user_reg.c_str(), &info) == 0 && (info.st_mode & 07777) == 0600);
  CHECK(fixture.registry_mutations == 2);
  host.Shutdown();

  // A title prepared before per-game names keeps the kit's name and icon: it
  // reports an update, and Update gives it the game's own without registering
  // again.
  std::error_code ignored;
  fs::remove_all(fixture.root / "data/z3cloudplay/games/steam-220/title-metadata", ignored);
  const auto title_param = fixture.root / "data/homebrew/FAKE78001/sce_sys/param.json";
  CHECK(chmod(title_param.c_str(), 0644) == 0);
  Write(title_param, Read(fixture.root / "kit/titles/variant/sce_sys/param.json"), 0644);
  auto named = config;
  named.title_icon = [](std::uint32_t) -> std::optional<std::string> { return std::string("game-icon-png"); };
  RuntimeHost upgraded(named);
  CHECK(upgraded.Start());
  CHECK(!WaitFor(upgraded, Started).installs[0].title_current);
  upgraded.Prepare(Hl2());
  const auto renamed = WaitFor(upgraded, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].title_current;
  }, 60);
  CHECK(!renamed.refusals.count(220));
  CHECK(Read(title_param).find("Half-Life") != std::string::npos);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/icon0.png") == "game-icon-png");
  CHECK(fixture.registry_mutations == 2);
  // Asking for the store name in titles makes the title out of date; Update
  // renames it, still without registering again.
  upgraded.SetTitleNames({{220, "Half-Life \"2\" (Steam)"}});
  WaitFor(upgraded, [](const RuntimeSnapshot& s) { return !s.installs.empty() && !s.installs[0].title_current; }, 10);
  auto suffixed = Hl2();
  suffixed.title_name = "Half-Life \"2\" (Steam)";
  upgraded.Prepare(suffixed);
  WaitFor(upgraded, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].title_current;
  }, 60);
  CHECK(Read(title_param).find(R"(Half-Life \"2\" (Steam))") != std::string::npos);
  CHECK(fixture.registry_mutations == 2);
  upgraded.Shutdown();
}

// Play from Z3CloudPlay: the worker checks the Ready title and prepares a session;
// only Handoff on the application thread starts it, once, with the session
// argument. The wrapper's exit record ends the session and allows the next Play.
void PlaysReadyTitleThroughHandoff() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.session_data_root = fixture.root / "data";
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::vector<std::pair<std::string, std::vector<std::string>>> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>& arguments) {
    launches.emplace_back(title, arguments);
    return TitleResult{true, 0, "launch"};
  };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  CHECK(!host.Handoff());  // nothing prepared
  host.Prepare(Hl2());
  WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);

  host.Play(220);
  const auto prepared = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(prepared.launch && prepared.launch->app_id == 220 && prepared.launch->title_id == "FAKE78001");
  // Cloud saves under Windows folders use this game's private bottle.
  CHECK(prepared.installs[0].prefix_root == "/data/z3cloudplay/games/steam-220/library/prefixes/game");
  CHECK(launches.empty());
  // Another thread cannot start the title.
  std::thread other([&host] { CHECK(host.Handoff()); });
  other.join();
  CHECK(launches.empty());
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff());
  CHECK(launches.size() == 1 && launches[0].first == "FAKE78001");
  const std::string session = "00000000-0000-4000-8000-000000000002";
  CHECK(launches[0].second == std::vector<std::string>{"session=" + session});
  CHECK(!host.Handoff());
  CHECK(host.Snapshot().launch->state == LaunchState::kLaunching);
  // A second Play while the session is undecided is refused and stays visible
  // for that game; the pending session is untouched.
  const auto handled = host.Snapshot().plays_handled;
  host.Play(220);
  const auto refused = WaitFor(host, [handled](const RuntimeSnapshot& s) { return s.plays_handled > handled; }, 30);
  CHECK(refused.play_refusals.count(220) && refused.launch && refused.launch->state == LaunchState::kLaunching);
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // one more poll
  CHECK(host.Snapshot().play_refusals.count(220) && launches.size() == 1);

  // The game cannot be updated or repaired while its session is undecided.
  host.Prepare(Hl2());
  const auto held = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; }, 30);
  CHECK(held.refusals.at(220).find("Close the game") != std::string::npos);
  // The wrapper records the session in the private game data root.
  const auto sessions = fixture.root / "data/z3cloudplay/games/steam-220/logs/sessions";
  fs::create_directories(sessions);
  CHECK(chmod((fixture.root / "data/z3cloudplay/games/steam-220/logs").c_str(), 0700) == 0);
  CHECK(chmod(sessions.c_str(), 0700) == 0);
  const auto record = [&](const std::string& id, const std::string& phase) {
    return R"({"schema_version":1,"session_id":")" + id +
           R"(","install_id":"steam-220","title_id":"FAKE78001","origin":"z3cloudplay","settings_revision":0,)"
           R"("runtime_bundle_id":"wine-wow64","phase":")" + phase + "\"}\n";
  };
  // Many earlier Home sessions do not hide this one.
  for (int i = 0; i < 300; ++i) {
    char id[37];
    std::snprintf(id, sizeof(id), "11111111-1111-4111-8111-%012d", i);
    Write(sessions / (std::string(id) + ".json"), record(id, "exited"), 0600);
  }
  Write(sessions / (session + ".json"), record(session, "running"), 0600);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kRunning; }, 30);
  Write(sessions / (session + ".json"), record(session, "exited"), 0600);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; }, 30);
  // The ended session no longer blocks Play, and an accepted Play clears the
  // earlier refusal.
  host.Play(220);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff());
  CHECK(launches.size() == 2 && launches[1].second[0] != launches[0].second[0]);
  CHECK(!host.Snapshot().play_refusals.count(220));
  // The system closes Z3CloudPlay when the game starts. If the game is then closed
  // from the system menu, its record still says running; the restarted host
  // ends that session once the title is seen stopped, and Play works again.
  const std::string third = "00000000-0000-4000-8000-000000000003";
  CHECK(launches[1].second[0] == "session=" + third);
  host.Shutdown();
  Write(sessions / (third + ".json"), record(third, "running"), 0600);
  RuntimeHost restarted(config);
  CHECK(restarted.Start());
  const auto ended = WaitFor(restarted, [](const RuntimeSnapshot& s) {
    return s.launch && s.launch->state == LaunchState::kExited;
  }, 30);
  CHECK(ended.launch->code == "title-ended-without-record");
  restarted.Play(220);
  WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(restarted.Handoff() && launches.size() == 3);
  // Evidence that does not belong to this session fails it, but while the title
  // runs the session stays pending and Play stays refused.
  const std::string fourth = "00000000-0000-4000-8000-000000000004";
  CHECK(launches[2].second[0] == "session=" + fourth);
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  // Cloud asks the console through the worker: a running title answers active,
  // and every answer carries a newer serial.
  const auto serial = restarted.Snapshot().activity_serial;
  restarted.CheckActivity(220);
  const auto checked = WaitFor(restarted, [serial](const RuntimeSnapshot& s) { return s.activity_serial > serial; }, 10);
  CHECK(checked.activity.at(220).first == NativeActivity::kActive && checked.activity.at(220).second > serial);
  restarted.CheckActivity(999);  // no prepared title: nothing of it can run
  const auto unprepared = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.activity.count(999) != 0; }, 10);
  CHECK(unprepared.activity.at(999).first == NativeActivity::kStopped);
  // Another thread (the Cloud worker) can wait for a fresh answer.
  CHECK(restarted.ActivityNow(220, std::chrono::seconds(10)) == NativeActivity::kActive);
  CHECK(restarted.ActivityNow(999, std::chrono::seconds(10)) == NativeActivity::kStopped);
  auto foreign = record(fourth, "running");
  foreign.replace(foreign.find("\"z3cloudplay\""), 9, "\"home\"");
  Write(sessions / (fourth + ".json"), foreign, 0600);
  WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kFailed; }, 30);
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const auto handled_before = restarted.Snapshot().plays_handled;
  restarted.Play(220);
  const auto refused_while_alive = WaitFor(restarted, [handled_before](const RuntimeSnapshot& s) {
    return s.plays_handled > handled_before;
  }, 30);
  CHECK(refused_while_alive.play_refusals.count(220) && launches.size() == 3);
  // Once the title is seen stopped, the failed session ends and Play works again.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  restarted.Play(220);
  WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(restarted.Handoff() && launches.size() == 4);
  restarted.Shutdown();
}

// A game Z3CloudPlay ships no recipe for is prepared from Steam's launch options; the
// generated recipe is saved and read back after a restart.
void PreparesGameWithoutShippedRecipe() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::pair<std::string, std::vector<std::string>>> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>& arguments) {
    launches.emplace_back(title, arguments);
    return TitleResult{true, 0, "launch"};
  };
  std::map<std::string, std::string> title_logs;
  config.title_log = [&title_logs](const std::string& title, const std::string& text) { title_logs[title] = text; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/4242";
  // A 64-bit PE header is all the generator reads.
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "Bin/Game.exe", pe, 0644);
  // The game's own Steam library (a 64-bit PE that asks for one interface).
  const std::string own_api = pe + std::string("\0SteamUser021\0", 14);
  Write(game / "Bin/steam_api64.dll", own_api, 0644);
  Write(game / ".z3cloudplay-steam", "depots=4243\ndlc=4250\n", 0644);
  std::map<std::string, std::string> saved_settings;
  config.settings_record = [&saved_settings](std::uint32_t) -> std::optional<GameSettingsRecord> {
    if (saved_settings.empty()) return std::nullopt;
    GameSettingsRecord record;
    record.overrides = saved_settings;
    return record;
  };
  CHECK(chmod(game.c_str(), 0755) == 0);
  RuntimePrepareRequest request{4242, "/data/homebrew/FAKE77031/games/4242", "internal", "Example Game"};

  RuntimeHost host(config);
  host.SetSteamAccount("player", 76561198000000001ull);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  // Without Steam's launch options there is nothing to start.
  request.launch_reason = "Steam details are unavailable right now.";
  host.Prepare(request);
  auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(4242) != 0; });
  CHECK(refused.refusals.at(4242) == "Steam details are unavailable right now.");
  CHECK(refused.installs.empty() && fixture.registry_mutations == 0);

  request.launch_reason.clear();
  request.launches = {{"", "", "bin\\game.exe", "", "-windowed"}};
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && ((s.refusals.count(4242) && s.refusals.at(4242) != "Steam details are unavailable right now.") ||
           (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(4242)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(4242).c_str());
  CHECK(done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady);
  CHECK(done.generated_recipes.size() == 1 && done.generated_recipes[0].id == "steam-4242-auto");
  CHECK(done.generated_recipes[0].architecture == "pe64");
  CHECK(done.generated_recipes[0].launch_entries[0].executable == "Bin/Game.exe");
  CHECK(fs::is_regular_file(fixture.root / "data/runtime-state/recipe-steam-4242-auto.json"));
  const auto descriptor = Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json");
  CHECK(descriptor.find("steam-4242-auto") != std::string::npos);
  // Steamworks: the emulator replaced the game's library, which is kept, and steam_settings/ was written.
  const auto emulator = Read(fs::path(g_runtime_dir) / "steamworks/steam_api64.dll");
  CHECK(Read(game / "Bin/steam_api64.dll") == emulator && Read(game / "Bin/steam_api64.dll.orig") == own_api);
  CHECK(Read(game / "Bin/steam_settings/configs.user.ini").find("account_name=player\n") != std::string::npos);
  CHECK(Read(game / "Bin/steam_settings/configs.app.ini").find("4250=dlc4250\n") != std::string::npos);
  CHECK(Read(game / "Bin/steam_settings/steam_interfaces.txt") == "SteamUser021\n");
  // The bottle's owner marker names the generated recipe, so the game's sessions are read.
  const auto marker = Read(fixture.root / "data/z3cloudplay/games/steam-4242/library/prefixes/game/.z3cloudplay-template-owner");
  CHECK(marker.find("\nrecipe_id=steam-4242-auto\n") != std::string::npos);
  // The player asks for the game's own Steam files: Play puts them back before the game starts.
  saved_settings = {{"steam_features", "game"}};
  host.Play(4242);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff() && launches.size() == 1 && launches[0].first == "FAKE78001");
  CHECK(Read(game / "Bin/steam_api64.dll") == own_api);
  const auto session = launches[0].second[0].substr(std::strlen("session="));
  const auto sessions = fixture.root / "data/z3cloudplay/games/steam-4242/logs/sessions";
  fs::create_directories(sessions);
  CHECK(chmod((fixture.root / "data/z3cloudplay/games/steam-4242/logs").c_str(), 0700) == 0);
  CHECK(chmod(sessions.c_str(), 0700) == 0);
  const auto record = [&](const std::string& phase) {
    return R"({"schema_version":1,"session_id":")" + session +
           R"(","install_id":"steam-4242","title_id":"FAKE78001","origin":"z3cloudplay","settings_revision":0,)"
           R"("runtime_bundle_id":"wine-wow64","phase":")" + phase + "\"}\n";
  };
  Write(sessions / (session + ".json"), record("running"), 0600);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kRunning; }, 30);
  Write(sessions / (session + ".json"), record("exited"), 0600);
  const auto ended = WaitFor(host, [](const RuntimeSnapshot& s) {
    return s.launch && s.launch->state != LaunchState::kRunning && s.launch->state != LaunchState::kLaunching;
  }, 30);
  CHECK(ended.launch->state == LaunchState::kExited);
  host.Shutdown();

  // After a restart the saved recipe is found without Steam, and the title log carries the
  // session report followed by the engine's own log from the bottle.
  const auto data = fixture.root / "data/z3cloudplay/games/steam-4242";
  Write(data / "library/logs/session-0.log", "PW_REPORT/1 stopped\n", 0600);
  Write(data / "library/prefixes/game/drive_c/users/prospero/AppData/LocalLow/Studio/Game/Player.log",
        "NullReferenceException at Boot\n", 0600);
  RuntimeHost restarted(config);
  CHECK(restarted.Start());
  const auto reloaded = WaitFor(restarted, [](const RuntimeSnapshot& s) { return Started(s) && !s.generated_recipes.empty(); });
  CHECK(reloaded.generated_recipes[0].id == "steam-4242-auto");
  CHECK(title_logs.count("FAKE78001"));
  const auto& title_log = title_logs.at("FAKE78001");
  CHECK(title_log.find("== session-0.log\nPW_REPORT/1 stopped") != std::string::npos);
  CHECK(title_log.find("AppData/LocalLow/Studio/Game/Player.log\nNullReferenceException at Boot") != std::string::npos);
  restarted.Prepare({4242, "/data/homebrew/FAKE77031/games/4242", "internal", "Example Game"});
  const auto prepared = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.refusals.count(4242) != 0; });
  CHECK(prepared.refusals.at(4242) == "This game is already prepared.");
  restarted.Shutdown();

  // Unity 5 writes output_log.txt beside the program instead; the newer log is the one shown.
  Write(game / "Bin/Game_Data/output_log.txt", "Initialize engine version: 5.3.6p5\n", 0600);
  std::filesystem::last_write_time(game / "Bin/Game_Data/output_log.txt",
                                   std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10));
  title_logs.clear();
  RuntimeHost legacy(config);
  CHECK(legacy.Start());
  WaitFor(legacy, [](const RuntimeSnapshot& s) { return Started(s) && !s.generated_recipes.empty(); });
  CHECK(title_logs.count("FAKE78001"));
  CHECK(title_logs.at("FAKE78001").find("== Game_Data/output_log.txt\nInitialize engine version: 5.3.6p5") !=
        std::string::npos);
  legacy.Prepare({4242, "/data/homebrew/FAKE77031/games/4242", "internal", "Example Game"});
  const auto again = WaitFor(legacy, [](const RuntimeSnapshot& s) { return s.refusals.count(4242) != 0; });
  CHECK(again.refusals.at(4242) == "This game is already prepared.");
  legacy.Shutdown();
}

// Settings saved while the game is being prepared, after its profile was written, reach the title
// as soon as it is ready instead of waiting for the next Z3CloudPlay start.
void SettingsSavedDuringPrepareReachReadyTitle() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  std::int64_t now = 1760000000;
  config.unix_time = [&now] { return now; };
  config.session_data_root = fixture.root / "data";
  std::mutex mutex;
  GameSettingsRecord saved;
  bool applied_logged = false;
  config.settings_record = [&](std::uint32_t) -> std::optional<GameSettingsRecord> {
    std::lock_guard<std::mutex> lock(mutex);
    return saved;
  };
  // The player changes a setting right after the profile is written.
  config.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    if (line.rfind("prepare step app=220 from=publish-profile", 0) == 0) {
      saved.revision = 1;
      saved.overrides = {{"resolution", "1280x720"}};
    }
    if (line == "prepared settings app=220 applied") applied_logged = true;
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  const auto profile = fixture.root / "data/z3cloudplay/games/steam-220/library/profiles/z3cloudplay-steam-220.profile";

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
  CHECK(done.installs[0].settings_revision == 1 && Read(profile).find("desktop = 1280x720") != std::string::npos);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(applied_logged);
  }
  host.Shutdown();
}

// Settings saved after preparation reach the Ready title through a journaled update, from
// maintenance or from Play, only while the title is stopped; maintenance clears the shader
// cache, backs up the bottle's user folders and repairs the bottle.
void MaintainsReadyTitle() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  bool loosen_next = false;  // the next armed update's capture sees a world-writable game folder
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  // Logged once the update is armed and before its capture: the seam for an interruption.
  unsigned full_inspections = 0, ownership_inspections = 0;
  config.log = [&loosen_next, game, &full_inspections, &ownership_inspections](const std::string& line) {
    if (line.rfind("package inspect ownership ", 0) == 0) ++ownership_inspections;
    else if (line.rfind("package inspect ", 0) == 0) ++full_inspections;
    if (loosen_next && line.rfind("settings update request", 0) == 0 && line.find(" state=0 ") != std::string::npos) {
      loosen_next = false;
      CHECK(chmod(game.c_str(), 0777) == 0);
    }
  };
  std::int64_t now = 1760000000;
  config.unix_time = [&now] { return now; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::string> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>&) {
    launches.push_back(title);
    return TitleResult{true, 0, "launch"};
  };
  GameSettingsRecord saved;
  config.settings_record = [&saved](std::uint32_t) -> std::optional<GameSettingsRecord> { return saved; };
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  const auto data = fixture.root / "data/z3cloudplay/games/steam-220";
  const auto profile = data / "library/profiles/z3cloudplay-steam-220.profile";

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  CHECK(!done.refusals.count(220) && done.installs[0].settings_revision == 0 && done.installs[0].launch_entry_id == "hl2");
  CHECK(Read(profile).find("desktop = 1920x1080") != std::string::npos && !fs::exists(game / "dxvk.conf"));

  const auto maintain = [&host](RuntimeMaintenance action) {
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, action);
    CHECK(host.Snapshot().maintenance.at(220).running);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  };
  // Saved while the game runs: nothing changes until it is stopped.
  saved.revision = 1;
  saved.overrides = {{"resolution", "1280x720"}, {"frame_limit", "30"}};
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  auto result = maintain(RuntimeMaintenance::kApplySettings);
  CHECK(!result.maintenance.at(220).ok && result.maintenance.at(220).message == "Changes apply the next time Z3CloudPlay starts the game.");
  CHECK(result.installs[0].settings_revision == 0 && Read(profile).find("desktop = 1920x1080") != std::string::npos);
  fixture.activity->store(static_cast<int>(NativeActivity::kUnknown));
  CHECK(!maintain(RuntimeMaintenance::kClearShaderCache).maintenance.at(220).ok);
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  full_inspections = ownership_inspections = 0;
  result = maintain(RuntimeMaintenance::kApplySettings);
  CHECK(result.maintenance.at(220).ok && result.maintenance.at(220).message == "Settings applied.");
  // The update checks the title's ownership once, before it changes anything, and never
  // walks its files and folders (14 full walks, then 11 ownership checks, before).
  CHECK(full_inspections == 0 && ownership_inspections == 1);
  CHECK(result.installs[0].settings_revision == 1 && !result.installs[0].recovery_pending);
  CHECK(Read(profile).find("desktop = 1280x720") != std::string::npos);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 30") != std::string::npos);
  CHECK(maintain(RuntimeMaintenance::kApplySettings).maintenance.at(220).message == "Settings are up to date.");

  // The shader cache of the launch program goes; nothing else in the folder does.
  Write(game / "hl2.dxvk-cache", "pipelines", 0644);
  CHECK(maintain(RuntimeMaintenance::kClearShaderCache).maintenance.at(220).ok);
  CHECK(!fs::exists(game / "hl2.dxvk-cache") && Read(game / "hl2.exe") == "existing-game");

  // Backups copy the Windows user folders; the newest three are kept.
  Write(data / "library/prefixes/game/drive_c/users/prospero/Documents/save.dat", "progress", 0600);
  for (int index = 0; index < 4; ++index, ++now) CHECK(maintain(RuntimeMaintenance::kBackupSaves).maintenance.at(220).ok);
  const auto backups = fixture.root / "data/z3cloudplay/games/steam-220.saves";
  CHECK(Read(backups / "1760000003/users/prospero/Documents/save.dat") == "progress");
  CHECK(!fs::exists(backups / "1760000000") && fs::exists(backups / "1760000001"));
  result = host.Snapshot();
  CHECK(result.installs[0].save_backups == 3 && result.installs[0].latest_save_backup == 1760000003);

  CHECK(chmod((data / "library/prefixes/game/user.reg").c_str(), 0666) == 0);
  result = maintain(RuntimeMaintenance::kRepair);
  CHECK(result.maintenance.at(220).ok);
  struct stat info{};
  CHECK(stat((data / "library/prefixes/game/user.reg").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);

  // An update interrupted after its arm keeps its journal: Play waits, and Resume (Prepare)
  // finishes the saved request instead of making a new one.
  saved.revision = 2;
  saved.overrides = {{"resolution", "1280x720"}, {"frame_limit", "60"}};
  loosen_next = true;
  result = maintain(RuntimeMaintenance::kApplySettings);
  CHECK(!result.maintenance.at(220).ok && result.installs[0].recovery_pending && result.installs[0].settings_revision == 1);
  const auto plays = host.Snapshot().plays_handled;
  host.Play(220);
  result = WaitFor(host, [plays](const RuntimeSnapshot& s) { return s.plays_handled > plays; }, 30);
  CHECK(result.play_refusals.count(220) && launches.empty());
  CHECK(chmod(game.c_str(), 0755) == 0);
  host.Prepare(Hl2());
  result = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (!s.installs[0].recovery_pending || s.refusals.count(220));
  }, 60);
  if (result.refusals.count(220)) std::fprintf(stderr, "Resume refused: %s\n", result.refusals.at(220).c_str());
  CHECK(!result.refusals.count(220) && !result.installs[0].recovery_pending && result.installs[0].settings_revision == 2);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 60") != std::string::npos);

  // Play publishes newer settings before the game starts, so Home starts the same profile.
  saved.revision = 3;
  saved.overrides = {{"resolution", "2560x1440"}};
  full_inspections = ownership_inspections = 0;
  host.Play(220);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff() && launches.size() == 1);
  // Play's checks: one full inspection for the launch (two before, plus a separate start
  // check), the apply's one ownership check and a marker-only native query.
  CHECK(full_inspections == 1 && ownership_inspections == 2);
  CHECK(host.Snapshot().installs[0].settings_revision == 3);
  CHECK(Read(profile).find("desktop = 2560x1440") != std::string::npos && !fs::exists(game / "dxvk.conf"));
  host.Shutdown();
}

// A kit respin while a settings update waits for recovery (#5): the title is still this
// install's, so Update first finishes the saved update, and the title only reports that
// it needs Update (not current) until a second Update refreshes it.
void KitRespinKeepsWaitingSettingsUpdate() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  bool loosen_next = false;  // interrupts the next armed update before its capture
  config.log = [&loosen_next, game](const std::string& line) {
    if (loosen_next && line.rfind("settings update request", 0) == 0 && line.find(" state=0 ") != std::string::npos) {
      loosen_next = false;
      CHECK(chmod(game.c_str(), 0777) == 0);
    }
  };
  GameSettingsRecord saved;
  config.settings_record = [&saved](std::uint32_t) -> std::optional<GameSettingsRecord> { return saved; };
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  {
    auto kit = SyntheticKit(fixture.root / "kit");
    kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    RuntimeHost host(first);
    CHECK(host.Start());
    host.Prepare(Hl2());
    const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    CHECK(ready.installs[0].title_current);
    saved.revision = 1;
    saved.overrides = {{"frame_limit", "30"}};
    loosen_next = true;
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, RuntimeMaintenance::kApplySettings);
    const auto interrupted = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
    CHECK(!interrupted.maintenance.at(220).ok && interrupted.installs[0].recovery_pending);
    host.Shutdown();
  }
  CHECK(chmod(game.c_str(), 0755) == 0);
  auto respun = SyntheticKit(fixture.root / "kit-respun", "native-2", "kit-1", 'c');
  respun.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [respun](std::string*) { return std::optional<RuntimeKit>(respun); };
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(!started.installs[0].title_current && started.installs[0].recovery_pending);
  host.Prepare(Hl2());
  auto result = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (!s.installs[0].recovery_pending || s.refusals.count(220));
  }, 60);
  if (result.refusals.count(220)) std::fprintf(stderr, "Resume after respin refused: %s\n", result.refusals.at(220).c_str());
  CHECK(!result.refusals.count(220) && result.installs[0].settings_revision == 1);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 30") != std::string::npos);
  CHECK(!result.installs[0].title_current);  // needs Update, not recovery
  host.Prepare(Hl2());
  result = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
  }, 60);
  CHECK(!result.refusals.count(220) && result.installs[0].title_current);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/eboot.bin") == "native-2");
  host.Shutdown();
}

// Uninstall removes the game's PS5 title: the title folder leaves ShadowMount's
// scan root, the title is unregistered through the remover, the bottle is kept
// (or deleted on request) and the record goes once the console no longer lists
// the title. Every step is journaled and continues after a failure or restart.
void RemovesTitleKeepingSaves() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-0000000000") + (uuids < 10 ? "0" : "") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.removal_confirm_polls = 3;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  // The remover: silent at first (the console never drops the title), then working.
  auto remover_works = std::make_shared<std::atomic<bool>>(false);
  auto remover_calls = std::make_shared<std::atomic<int>>(0);
  config.unregister_title = [&fixture, remover_works, remover_calls](const std::string& title, const std::string& nonce) {
    ++*remover_calls;
    CHECK(title == "FAKE78001" && nonce.size() == 36);
    if (!*remover_works) return TitleRemoverResult{true, std::nullopt, "no answer"};
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  const auto bottle = fixture.root / "data/z3cloudplay/games/steam-220";
  const auto kept = fixture.root / "data/z3cloudplay/games/steam-220.kept";
  const auto save = fs::path("library/prefixes/game/drive_c/users/steamuser/AppData/Roaming/Game/save.dat");
  const auto prepare = [](RuntimeHost& host) {
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs.size() == 1);
    return done;
  };
  const auto remove = [](RuntimeHost& host, bool delete_saves) {
    const auto before = host.Snapshot().removals_handled;
    host.Remove(220, delete_saves);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.removals_handled > before; }, 30);
  };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    prepare(host);
    host.SetTitleNames({{220, "Half-Life 2"}});
    Write(bottle / save, "player-save", 0600);
    for (const auto* part : {"library/prefixes/game/drive_c/users", "library/prefixes/game/drive_c/users/steamuser",
                             "library/prefixes/game/drive_c/users/steamuser/AppData",
                             "library/prefixes/game/drive_c/users/steamuser/AppData/Roaming",
                             "library/prefixes/game/drive_c/users/steamuser/AppData/Roaming/Game"})
      CHECK(chmod((bottle / part).c_str(), 0700) == 0);

    // A running title is never removed.
    fixture.activity->store(static_cast<int>(NativeActivity::kActive));
    auto removed = remove(host, false);
    CHECK(removed.removal_failures.at(220) == "Close the game before uninstalling it.");
    CHECK(removed.installs[0].state == PreparationState::kReady && *remover_calls == 0);
    fixture.activity->store(static_cast<int>(NativeActivity::kStopped));

    // The remover is silent: the folder already left the scan root, nothing is
    // recorded as failed by the console, and the record waits at "unregister".
    removed = remove(host, false);
    CHECK(removed.removal_failures.at(220).find("has not confirmed") != std::string::npos);
    CHECK(removed.installs.size() == 1 && removed.installs[0].state == PreparationState::kRemoving);
    CHECK(removed.installs[0].step == "unregister");
    CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001"));
    CHECK(fs::is_directory(fixture.root / "data/runtime-state/trash/FAKE78001-00000000-0000-4000-8000-000000000002"));
    CHECK(*remover_calls == 1);
    // Preparation waits for the removal to finish.
    host.Prepare(Hl2());
    const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
    CHECK(refused.refusals.at(220).find("still being removed") != std::string::npos);
    host.Shutdown();
  }
  {
    // After a restart the removal is reported, not resumed, until asked. Asked again,
    // it continues with the first choice (keep saves) even when asked to delete them.
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(started.installs.size() == 1 && started.installs[0].state == PreparationState::kRemoving);
    CHECK(started.installs[0].failure.find("has not confirmed") != std::string::npos);
    CHECK(!fs::exists(fixture.root / "data/runtime-state/trash/FAKE78001-00000000-0000-4000-8000-000000000002"));
    // The PS5 title carries the store name; the kept saves are named for the game.
    host.SetTitleNames({{220, "Half-Life 2 (Steam)"}});
    // A read-only games folder stops the bottle step; the next attempt continues there.
    const auto games = fixture.root / "data/z3cloudplay/games";
    CHECK(chmod(games.c_str(), 0500) == 0);
    *remover_works = true;
    auto removed = remove(host, true);
    CHECK(chmod(games.c_str(), 0700) == 0);
    CHECK(removed.installs.size() == 1 && removed.installs[0].step == "bottle");
    CHECK(removed.removal_failures.at(220).find("could not be kept") != std::string::npos);
    CHECK(!fixture.registered && *remover_calls == 2);
    removed = remove(host, true);
    CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
    CHECK(*remover_calls == 2);  // the console no longer listed the title: never sent again
    CHECK(!fs::exists(bottle) && Read(kept / save) == "player-save");
    CHECK(removed.kept_bottles.size() == 1 && removed.kept_bottles[0].app_id == 220);
    CHECK(removed.kept_bottles[0].name == "Half-Life 2" && removed.kept_bottles[0].kept_at == 1760000000);
    CHECK(removed.kept_bottles[0].bytes > 0);
    CHECK(Read(kept / "z3cloudplay-kept.json").find("\"install_id\":\"steam-220\"") != std::string::npos);
    CHECK(Read(game / "hl2.exe") == "existing-game");  // the download is the controller's to remove
    // The kept bottle says where it is, and its Windows user folders can be measured (the Saves tab).
    CHECK(removed.kept_bottles[0].data_root.size() > 14 &&
          removed.kept_bottles[0].data_root.compare(removed.kept_bottles[0].data_root.size() - 14, 14, "steam-220.kept") == 0);
    host.MeasureSaves();
    const auto measured = WaitFor(host, [](const RuntimeSnapshot& s) { return s.save_bytes.count(220) > 0; });
    CHECK(measured.save_bytes.at(220) >= std::string("player-save").size());

    // Prepared again, the game gets its saves back and the kept copy goes.
    const auto ready = prepare(host);
    CHECK(ready.kept_bottles.empty() && !fs::exists(kept));
    CHECK(!ready.work_progress);  // the copy's progress ends with the preparation
    CHECK(Read(bottle / save) == "player-save");
    struct stat info{};
    CHECK(stat((bottle / "library/prefixes/game/drive_c/users/steamuser").c_str(), &info) == 0 &&
          (info.st_mode & 0777) == 0700);

    // Deleting saves removes the bottle without following its links.
    const auto outside = fixture.root / "outside";
    Write(outside / "keep.txt", "not the bottle's", 0644);
    fs::create_directories(bottle / "library/prefixes/game/dosdevices");
    fs::create_directory_symlink(outside, bottle / "library/prefixes/game/dosdevices/z:");
    removed = remove(host, true);
    CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
    CHECK(!fs::exists(bottle) && !fs::exists(kept) && removed.kept_bottles.empty());
    CHECK(!removed.work_progress);  // the deletion's progress ends with it
    CHECK(Read(outside / "keep.txt") == "not the bottle's");

    // A kept bottle can be deleted on its own.
    prepare(host);
    removed = remove(host, false);
    CHECK(removed.kept_bottles.size() == 1);
    host.DeleteKeptBottle(220);
    WaitFor(host, [](const RuntimeSnapshot& s) { return s.kept_bottles.empty(); });
    CHECK(!fs::exists(kept));
    host.Shutdown();
  }
}

// A game on a drive that records no file owners (exFAT) keeps its folder as it
// is. Its publication says so (metadata schema 3), and only a kit that can start
// games from such drives prepares it.
void PreparesGameOnUnownedDrive() {
  z3cloudplay::platform::SetOwnershipProbeForTesting([](const fs::path&) { return std::optional<bool>(false); });
  for (const bool capable : {false, true}) {
    Fixture fixture;
    auto config = fixture.Config();
    config.host_namespace_prefix = fixture.root;
    config.package_paths.console_data_root = "/data";
    config.package_paths.host_data_root = fixture.root / "data";
    config.package_operations.hash_file = HashArtifactFile;
    config.package_operations.hash_bytes = HashArtifactBytes;
    config.package_operations.free_space_unverifiable = true;
    config.package_operations.validate_bottle = [](const fs::path& bottle) {
      return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
    };
    auto kit = SyntheticKit(fixture.root / "kit");
    if (capable) {
      kit.capabilities.push_back("game-root-unowned");
      std::sort(kit.capabilities.begin(), kit.capabilities.end());
    }
    config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    unsigned uuids = 0;
    config.uuid = [&uuids]() -> std::optional<std::string> {
      ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
    };
    config.unix_time = [] { return std::int64_t{1760000000}; };
    const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
    Write(game / "hl2.exe", "existing-game", 0644);
    CHECK(chmod(game.c_str(), 0777) == 0);  // what exFAT reports for every folder

    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    struct stat game_info{};
    CHECK(stat(game.c_str(), &game_info) == 0 && (game_info.st_mode & 07777) == 0777);
    if (!capable) {
      CHECK(done.refusals.count(220) && done.refusals.at(220).find("doesn't keep file owners") != std::string::npos);
      CHECK(done.installs.empty());
    } else {
      if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
      CHECK(!done.refusals.count(220));
      CHECK(done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady);
      const auto metadata = Read(fixture.root / "data/z3cloudplay/games/steam-220/library/profiles/.z3cloudplay-publication");
      CHECK(metadata.find("schema=3\n") != std::string::npos);
      CHECK(metadata.find("game_root.unowned=1\n") != std::string::npos);
    }
    host.Shutdown();
  }
  z3cloudplay::platform::SetOwnershipProbeForTesting(nullptr);
}

// Selaco (issue 52): Steam lists only .bat files, so Z3CloudPlay refuses it, but the program the
// player picks in Game settings prepares and starts; later settings updates keep that program.
void PreparesProgramChosenInSettings() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/1592280";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "SELACO.exe", pe, 0644);
  Write(game / "Launch.bat", "start Launcher.exe\n", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  GameSettingsRecord saved;
  bool has_record = false;
  config.settings_record = [&](std::uint32_t) -> std::optional<GameSettingsRecord> {
    if (!has_record) return std::nullopt;
    return saved;
  };
  RuntimePrepareRequest request{1592280, "/data/homebrew/FAKE77031/games/1592280", "internal", "Selaco"};
  request.launches = {{"", "Play", "Launch.bat", "", ""}};
  const auto profile = fixture.root / "data/z3cloudplay/games/steam-1592280/library/profiles/z3cloudplay-steam-1592280.profile";

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(1592280) != 0; });
  CHECK(refused.refusals.at(1592280).find("no Windows program") != std::string::npos && refused.installs.empty());
  // Game settings gets a draft to start from and the folder's programs to pick.
  const auto& setup = refused.game_setups.at(1592280);
  CHECK(setup.draft && setup.draft->launch_entries.empty() && setup.refusal_soft && setup.scanned);
  CHECK(setup.programs.size() == 1 && setup.programs[0].path == "SELACO.exe" && setup.programs[0].architecture == "pe64");

  // The player picks SELACO.exe: Prepare makes the recipe from it and the title starts it.
  has_record = true;
  saved.overrides = {{"program", "SELACO.exe"}, {"architecture", "pe64"}};
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);
  CHECK(!done.refusals.count(1592280) && !done.game_setups.at(1592280).draft);
  CHECK(done.generated_recipes.size() == 1 && done.generated_recipes[0].launch_entries[0].id == kProgramEntryId);
  CHECK(Read(profile).find("\\games\\1592280\\SELACO.exe\n") != std::string::npos);
  CHECK(Read(profile).find("\narchitecture = pe64\n") != std::string::npos);

  // A later update applies the program's folder too, without preparing the game again; a folder
  // that isn't there is refused and the title keeps what it had.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const auto apply = [&](std::uint64_t revision) {
    saved.revision = revision;
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(1592280, RuntimeMaintenance::kApplySettings);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  };
  saved.overrides["program_arguments"] = "-nostartup";
  saved.overrides["program_folder"] = "Elsewhere";
  auto applied = apply(1);
  CHECK(!applied.maintenance.at(1592280).ok &&
        applied.maintenance.at(1592280).message.find("Elsewhere chosen in Game settings is missing") != std::string::npos);
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\1592280\n") != std::string::npos);
  CHECK(applied.installs[0].launch_working_directory.empty() && applied.installs[0].launch_path == "SELACO.exe" &&
        !applied.installs[0].recovery_pending);
  std::filesystem::create_directory(game / "Elsewhere");
  CHECK(chmod((game / "Elsewhere").c_str(), 0755) == 0);
  applied = apply(2);
  if (!applied.maintenance.at(1592280).ok) std::fprintf(stderr, "apply: %s\n", applied.maintenance.at(1592280).message.c_str());
  CHECK(applied.maintenance.at(1592280).ok);
  CHECK(Read(profile).find("\narguments = -nostartup\n") != std::string::npos);
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\1592280\\Elsewhere\n") !=
        std::string::npos);
  CHECK(applied.installs[0].launch_working_directory == "Elsewhere");
  // Another program from the folder: the profile and the install record both name it.
  Write(game / "Other.exe", pe, 0644);
  saved.overrides = {{"program", "Other.exe"}, {"architecture", "pe64"}};
  applied = apply(3);
  if (!applied.maintenance.at(1592280).ok) std::fprintf(stderr, "switch: %s\n", applied.maintenance.at(1592280).message.c_str());
  CHECK(applied.maintenance.at(1592280).ok);
  CHECK(Read(profile).find("\\games\\1592280\\Other.exe\n") != std::string::npos);
  CHECK(applied.installs[0].launch_path == "Other.exe" && applied.installs[0].launch_entry_id == kProgramEntryId &&
        applied.installs[0].launch_working_directory.empty() && applied.installs[0].settings_revision == 3);
  // A program that isn't there is refused before anything changes.
  saved.overrides = {{"program", "Missing.exe"}, {"architecture", "pe64"}};
  applied = apply(4);
  CHECK(!applied.maintenance.at(1592280).ok && applied.maintenance.at(1592280).message.find("missing") != std::string::npos);
  CHECK(Read(profile).find("\\games\\1592280\\Other.exe\n") != std::string::npos &&
        applied.installs[0].launch_path == "Other.exe" && applied.installs[0].settings_revision == 3);
  host.Shutdown();
}

// An Unreal Engine 3 (UDK) game asks for its EULA through UnSetup.exe, which the PS5 cannot
// start; preparing it marks the EULA accepted in the bottle's system.reg instead, once.
void PreparesUdkGameWithEulaAccepted() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  config.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  };
  const auto eula_lines = [&] {
    std::lock_guard<std::mutex> lock(lines_mutex);
    std::vector<std::string> found;
    for (const auto& line : lines)
      if (line.rfind("ue3 eula ", 0) == 0) found.push_back(line);
    return found;
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/219890";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x4c; pe[0x45] = 0x01; pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x01;
  Write(game / "Binaries/Win32/UDK.exe", pe, 0644);
  Write(game / "Binaries/UnSetup.exe", "MZ", 0644);
  Write(game / "Binaries/InstallInfo.xml",
        "<?xml version=\"1.0\"?>\r\n<InstallInfo>\r\n  <InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933"
        "</InstallGuidString>\r\n</InstallInfo>", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  RuntimePrepareRequest request{219890, "/data/homebrew/FAKE77031/games/219890", "internal", "Antichamber"};
  request.launches = {{"", "Play", "Binaries\\Win32\\UDK.exe", "", ""}};

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(219890) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(219890)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(219890).c_str());
  CHECK(!done.refusals.count(219890) && done.installs.size() == 1);
  const auto bottle = fixture.root / "data/z3cloudplay/games/steam-219890/library/prefixes/game";
  const auto key = std::string("Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\UDK-dcf140f3-829e-4033-950d-b3be8d76b933]");
  const auto system_reg = Read(bottle / "system.reg");
  CHECK(system_reg.find("[Software\\\\" + key) != std::string::npos);
  CHECK(system_reg.find("[Software\\\\Wow6432Node\\\\" + key) != std::string::npos);
  CHECK(Read(bottle / "system.reg.z3cloudplay-backup") == "WINE REGISTRY Version 2\n");
  auto logged = eula_lines();
  CHECK(!logged.empty() && logged.front() == "ue3 eula app=219890 guid=dcf140f3-829e-4033-950d-b3be8d76b933 written=1");

  // Repair runs the fixups again: the value is already there, so nothing is rewritten.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const auto before = host.Snapshot().maintenance_handled;
  host.Maintain(219890, RuntimeMaintenance::kRepair);
  const auto repaired = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  if (!repaired.maintenance.at(219890).ok) std::fprintf(stderr, "repair: %s\n", repaired.maintenance.at(219890).message.c_str());
  CHECK(repaired.maintenance.at(219890).ok);
  CHECK(Read(bottle / "system.reg") == system_reg);
  logged = eula_lines();
  CHECK(logged.back() == "ue3 eula app=219890 guid=dcf140f3-829e-4033-950d-b3be8d76b933 written=0");
  host.Shutdown();
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_runtime_dir = argv[1];
  NoKitRefusesPreparationWithoutDurableEffects();
  QueuedPreparationIsPublishedUntilTaken();
  StartupReportsJournalsWithoutResuming();
  PublicStateRootIsUnavailable();
  PreparesExistingDownloadToReady();
  PreparesGameOnUnownedDrive();
  AnyTitleKitMakesTheGameItsOwnTitle();
  PreparationEndsAwaitingRegistration();
  PreparesGameWithoutShippedRecipe();
  PreparesProgramChosenInSettings();
  PreparesUdkGameWithEulaAccepted();
  ResumesInterruptedCaptureFromSavedRequest();
  ResumesInterruptedCaptureFromSavedRequest(true);
  RefreshesInstalledTitleToRespunKit();
  RepairsBottleRegistryModes();
  PlaysReadyTitleThroughHandoff();
  MaintainsReadyTitle();
  SettingsSavedDuringPrepareReachReadyTitle();
  KitRespinKeepsWaitingSettingsUpdate();
  RemovesTitleKeepingSaves();
  std::puts("runtime host tests passed");
  return 0;
}
