// runtime_host.cpp - Worker-owned composition of the per-game runtime services.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include "homebrew/runtime_host.hpp"

#include "platform/storage.h"
#include "runtime/private_directories.h"
#include "runtime/prospero_profile.h"
#include "runtime/settings_publication_journal.h"
#include "runtime/dotnet_heap.h"
#include "runtime/install_script.h"
#include "runtime/settings_publication_owner.h"
#include "runtime/session_storage.h"
#include "runtime/steam_stub.h"
#include "runtime/steamworks.h"
#include "steam/backend/cloud_mapping.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <set>
#include <string_view>
#include <utility>

namespace z3cloudplay::homebrew {
namespace rt = z3cloudplay::runtime;
namespace {
// Stands in for the package layer while no verified kit is admitted. It reports
// unknown package state and refuses every action, so nothing is staged.
class UnavailablePackageInstaller final : public rt::PackageInstaller {
 public:
  explicit UnavailablePackageInstaller(std::string reason) : reason_(std::move(reason)) {}
  rt::PackageStatus Inspect(const rt::RuntimeInstall&) override { return {}; }
  rt::PackageActionResult StageTitle(const rt::RuntimeInstall&) override { return Refuse(); }
  rt::PackageActionResult CopyTemplate(const rt::RuntimeInstall&) override { return Refuse(); }
  rt::PackageActionResult ValidateArtifacts(const rt::RuntimeInstall&) override { return Refuse(); }
  rt::PackageActionResult PublishProfile(const rt::RuntimeInstall&) override { return Refuse(); }
 private:
  rt::PackageActionResult Refuse() const { return {rt::PackageActionState::kFailed, "kit-unavailable", reason_}; }
  std::string reason_;
};

constexpr int kMaxPreparationSteps = 64;
// A pending launch is polled at this interval, and Handoff trusts a preflight
// the worker made at most this long ago.
constexpr auto kLaunchPollInterval = std::chrono::seconds(2);
constexpr std::int64_t kPreflightFreshMs = 5000;

std::string InstallId(std::uint32_t app_id) { return "steam-" + std::to_string(app_id); }

// Staged, its registration submitted, and only waiting for ShadowMountPlus, which
// registers titles once no title runs: the game is ready to play from Home. A
// registration the console refused (a stale hook, a failed submission) is not.
bool AwaitingRegistration(const rt::RuntimeInstall& record) {
  return record.journal && (record.journal->step == "register" || record.journal->step == "await-native") &&
         (!record.last_failure || record.last_failure->code == "registration-uncertain");
}
}  // namespace

// Newest wrapper session log under <data root>/library/logs, first 32 KiB and last 128 KiB.
// The first head and last tail bytes of a regular file, without following links; a
// marker line stands for what lies between them.
std::string FileTail(const std::filesystem::path& path, off_t tail, off_t head = 0) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return {};
  struct stat info{};
  std::string bytes;
  const auto read_range = [&](off_t start, off_t length) {
    std::string part(static_cast<std::size_t>(length), '\0');
    std::size_t done = 0;
    while (done < part.size()) {
      const auto count = pread(fd, part.data() + done, part.size() - done, start + static_cast<off_t>(done));
      if (count <= 0) break;
      done += static_cast<std::size_t>(count);
    }
    part.resize(done);
    return part;
  };
  if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
    if (info.st_size > head + tail && head > 0)
      bytes = read_range(0, head) + "\n== ... " + std::to_string(info.st_size - head - tail) + " bytes left out ...\n";
    const off_t start = info.st_size > tail + head ? info.st_size - tail : (head > 0 ? 0 : std::max<off_t>(0, info.st_size - tail));
    bytes += read_range(start, info.st_size - start);
  }
  close(fd);
  return bytes;
}

// The newest regular file in folder whose name passes keep. Links are skipped.
template <typename Keep>
void Newest(const std::filesystem::path& folder, Keep keep, std::filesystem::path* newest,
            std::filesystem::file_time_type* newest_time) {
  namespace fs = std::filesystem;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    if (!keep(it->path().filename().string()) || it->is_symlink(error) || !it->is_regular_file(error)) continue;
    const auto when = it->last_write_time(error);
    if (!error && (newest->empty() || when > *newest_time)) { *newest = it->path(); *newest_time = when; }
  }
}

// The subfolders of folder, links skipped.
std::vector<std::filesystem::path> Folders(const std::filesystem::path& folder) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), end; !error && it != end && out.size() < 256; it.increment(error))
    if (!it->is_symlink(error) && it->is_directory(error)) out.push_back(it->path());
  return out;
}

// The newest session report of a game's bottle, followed by the newest log its engine wrote:
// Unity's AppData/LocalLow/<company>/<product>/Player.log or Unreal's
// AppData/Local/<project>/Saved/Logs/*.log in the bottle, or Unity 5's
// <program>_Data/output_log.txt beside the program. Those name the crash when the report
// stops short.
std::optional<std::string> NewestSessionLog(const std::filesystem::path& data_root,
                                            const std::filesystem::path& program) {
  namespace fs = std::filesystem;
  fs::path newest; fs::file_time_type newest_time{};
  Newest(data_root / "library/logs", [](const std::string& name) { return name.rfind("session-", 0) == 0; },
         &newest, &newest_time);
  if (newest.empty()) return std::nullopt;
  std::string text = "== " + newest.filename().string() + "\n" + FileTail(newest, 128 * 1024, 32 * 1024);
  // Every report in the folder, newest first, and the start of the one before the newest: a run
  // that wrote little, or a rotated report, still shows which runs there were and how they began.
  {
    std::vector<std::pair<fs::file_time_type, fs::path>> reports;
    std::error_code error;
    for (fs::directory_iterator it(data_root / "library/logs", error), end; !error && it != end && reports.size() < 64;
         it.increment(error))
      if (it->path().filename().string().rfind("session-", 0) == 0 && !it->is_symlink(error) && it->is_regular_file(error))
        reports.emplace_back(it->last_write_time(error), it->path());
    std::sort(reports.begin(), reports.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    text += "\n== reports";
    for (const auto& [when, path] : reports) {
      std::error_code size_error;
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
      text += "\n" + path.filename().string() + " bytes=" + std::to_string(fs::file_size(path, size_error)) +
              " time=" + std::to_string(seconds);
    }
    if (reports.size() > 1) text += "\n== " + reports[1].second.filename().string() + " (start)\n" + FileTail(reports[1].second, 0, 32 * 1024);
  }
  const auto user = data_root / "library/prefixes/game" / z3cloudplay::steam::cloud::kPrefixUserFolder / "AppData";
  fs::path engine; fs::file_time_type engine_time{};
  for (const auto& company : Folders(user / "LocalLow"))
    for (const auto& product : Folders(company))
      Newest(product, [](const std::string& name) { return name == "Player.log"; }, &engine, &engine_time);
  for (const auto& project : Folders(user / "Local"))
    Newest(project / "Saved/Logs", [](const std::string& name) {
      return name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0;
    }, &engine, &engine_time);
  if (!program.empty())
    Newest(program.parent_path() / (program.stem().string() + "_Data"),
           [](const std::string& name) { return name == "output_log.txt"; }, &engine, &engine_time);
  if (!engine.empty()) {
    const auto shown = engine.lexically_relative(data_root);
    text += "\n== " + (shown.empty() || *shown.begin() == ".." ? engine.parent_path().filename() / engine.filename() : shown).string() + "\n" +
            FileTail(engine, 64 * 1024);
  }
  return text;
}

std::optional<std::string> RandomUuid() {
  unsigned char bytes[16];
  const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::size_t read_bytes = 0;
  while (read_bytes < sizeof(bytes)) {
    const auto count = read(fd, bytes + read_bytes, sizeof(bytes) - read_bytes);
    if (count <= 0) { close(fd); return std::nullopt; }
    read_bytes += static_cast<std::size_t>(count);
  }
  close(fd);
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);  // version 4
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);  // RFC 4122 variant
  char text[37];
  std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return std::string(text);
}

// Construction order resolves the ownership cycle:
//   repository -> records -> installer -> registry -> coordinator -> owner -> service.
// The repository's availability query consults the coordinator, which is built
// later; the repository constructor never calls it, and Load/Reserve/Adopt only
// run after the whole composition exists. The installer's publication callback and
// the service's driver dereference the owner at call time. Members are destroyed
// in reverse order, so no collaborator outlives what it references.
struct RuntimeHost::Composition {
  explicit Composition(const RuntimeHostConfig& config)
      : storage(config.state_root.string()),
        repository(storage, "installs.json",
                   [this](const std::string& title) {
                     return coordinator ? coordinator->Availability(title) : rt::NativeTitleAvailability{};
                   }),
        records(repository),
        registry(config.registry_transport) {}

  z3cloudplay::Storage storage;
  // PS5 titles removed while Z3CloudPlay runs: the console keeps their registration until Z3CloudPlay
  // quits, so a new game reusing one fails publication ("ownership is unavailable or incoherent").
  std::set<std::string> retired_titles;
  rt::RuntimeInstallRepository repository;
  rt::RepositoryInstallRecordStore records;
  std::optional<RuntimeKit> kit;
  std::unique_ptr<rt::PackageInstaller> installer;
  ps5::TitleRegistry registry;
  std::unique_ptr<ps5::Ps5NativeCoordinator> coordinator;
  std::unique_ptr<rt::SettingsPublicationOwner> owner;
  std::unique_ptr<rt::RuntimeInstallService> service;
  std::filesystem::path request_directory;
  std::map<std::string, std::string> admission;  // install id -> last admission diagnostic
  std::vector<rt::GameRecipe> recipes;
  std::vector<rt::SettingsPreset> presets;
  std::string settings_data_reason;
  std::unique_ptr<rt::StorageLaunchSessionJournal> launch_journal;
  std::unique_ptr<rt::FilesystemReconcileSessionSource> sessions;
  std::unique_ptr<rt::LaunchService> launch;
  // Last worker preflight per install, with its time, for Handoff.
  std::map<std::string, std::pair<rt::LaunchPreflight, std::int64_t>> preflights;
};

RuntimeHost::RuntimeHost(RuntimeHostConfig config) : config_(std::move(config)) {
  if (!config_.uuid) config_.uuid = RandomUuid;
  if (!config_.unix_time) config_.unix_time = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
  if (!config_.now_ms)
    config_.now_ms = [] {
      return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
    };
  application_thread_ = pthread_self();
}
RuntimeHost::~RuntimeHost() { Shutdown(); }

bool RuntimeHost::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) return true;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  auto* thread = new pthread_t{};
  const bool ok = pthread_attr_setstacksize(&attributes, config_.stack_bytes) == 0 &&
                  pthread_create(thread, &attributes, &RuntimeHost::Entry, this) == 0;
  pthread_attr_destroy(&attributes);
  if (!ok) {
    delete thread;
    snapshot_.status = RuntimeHostStatus::kUnavailable;
    snapshot_.reason = "The runtime worker could not start.";
    ++snapshot_.revision;
    return false;
  }
  thread_ = thread;
  started_ = true;
  return true;
}

void RuntimeHost::Shutdown() {
  pthread_t* thread = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    thread = static_cast<pthread_t*>(thread_);
    thread_ = nullptr;
  }
  wake_.notify_all();
  published_.notify_all();
  if (thread) {
    pthread_join(*thread, nullptr);
    delete thread;
  }
}

RuntimeSnapshot RuntimeHost::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

std::shared_ptr<const RuntimeSnapshot> RuntimeHost::Shared() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!shared_ || shared_revision_ != snapshot_.revision) {
    shared_ = std::make_shared<const RuntimeSnapshot>(snapshot_);
    shared_revision_ = snapshot_.revision;
  }
  return shared_;
}

void RuntimeHost::Log(const std::string& line) const {
  if (config_.log) config_.log(line);
}

bool RuntimeHost::Stopping() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stopping_;
}

void RuntimeHost::Prepare(RuntimePrepareRequest request) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    snapshot_.queued_prepares.insert(request.app_id);
    ++snapshot_.revision;
    prepare_requests_.push_back(std::move(request));
  }
  published_.notify_all();
  wake_.notify_all();
}

void RuntimeHost::Play(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    play_requests_.push_back(app_id);
  }
  wake_.notify_all();
}

void RuntimeHost::Remove(std::uint32_t app_id, bool delete_saves) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    remove_requests_.emplace_back(app_id, delete_saves);
  }
  wake_.notify_all();
}

void RuntimeHost::DeleteKeptBottle(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    kept_delete_requests_.push_back(app_id);
  }
  wake_.notify_all();
}

void RuntimeHost::ScanPrograms(std::uint32_t app_id, std::string install_path) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    scan_requests_.emplace_back(app_id, std::move(install_path));
  }
  wake_.notify_all();
}

void RuntimeHost::MeasureSaves() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    measure_requested_ = true;
  }
  wake_.notify_all();
}

void RuntimeHost::Maintain(std::uint32_t app_id, RuntimeMaintenance action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    maintenance_requests_.emplace_back(app_id, action);
    snapshot_.maintenance[app_id] = {action, true, false, {}};
    ++snapshot_.revision;
  }
  wake_.notify_all();
}

void RuntimeHost::SetSteamAccount(std::string account_name, std::uint64_t steam_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  steam_account_name_ = std::move(account_name);
  steam_id_ = steam_id;
}

std::string RuntimeHost::ApplySteamworks(Composition& c, const rt::RuntimeInstall& record) {
  const rt::GameRecipe* recipe = nullptr;
  for (const auto& candidate : c.recipes)
    if (candidate.app_id == record.app_id && candidate.id == record.recipe_id) recipe = &candidate;
  if (!recipe) return {};  // publication reports the missing recipe
  const auto saved = config_.settings_record ? config_.settings_record(record.app_id) : std::nullopt;
  const rt::SettingsPreset* preset = nullptr;
  if (saved)
    for (const auto& candidate : c.presets)
      if (candidate.id == saved->preset_id) preset = &candidate;
  rt::EffectiveSettings settings;
  rt::SettingsError error;
  static const std::map<std::string, std::string> none;
  const auto kit = c.kit ? rt::KitCapabilitiesFromManifest(c.kit->capabilities, true) : rt::KitCapabilities{};
  if (!rt::BuildEffectiveSettings(*recipe, preset, saved ? saved->overrides : none, kit, &settings, &error))
    return {};  // publication reports invalid settings
  const auto root = config_.host_namespace_prefix.empty()
      ? std::filesystem::path(record.install_path)
      : config_.host_namespace_prefix / std::filesystem::path(record.install_path).relative_path();
  // A program wrapped in SteamStub can't start without the Steam client, whichever Steam
  // library the game uses, so it is unpacked first (runtime/steam_stub.h).
  std::vector<std::string> programs;
  for (const auto& entry : recipe->launch_entries) {
    auto program = entry.executable;
    std::replace(program.begin(), program.end(), '\\', '/');
    programs.push_back(std::move(program));
  }
  // A program picked from the game folder may not be one of the recipe's.
  if (auto own = record.launch_path; !own.empty()) {
    std::replace(own.begin(), own.end(), '\\', '/');
    if (std::find(programs.begin(), programs.end(), own) == programs.end()) programs.push_back(std::move(own));
  }
  const auto unpacked = rt::UnpackSteamStubPrograms(root.string(), programs);
  if (!unpacked.found.empty() || !unpacked.ok) {
    std::string found, changed;
    for (const auto& path : unpacked.found) found += (found.empty() ? "" : ",") + path;
    for (const auto& path : unpacked.changed) changed += (changed.empty() ? "" : ",") + path;
    Log("steamstub app=" + std::to_string(record.app_id) + " found=" + found + " changed=" + (changed.empty() ? "-" : changed) +
        (unpacked.ok ? std::string() : " failed=" + unpacked.message));
  }
  if (!unpacked.ok) return unpacked.message;
  const bool emulate = settings.values.at("steam_features").value == "emulate";
  rt::SteamworksResult result;
  if (emulate) {
    rt::SteamworksIdentity identity;
    identity.app_id = record.app_id;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      identity.account_name = steam_account_name_;
      identity.steam_id = steam_id_;
    }
    if (!rt::ReadSteamworksFacts(root.string(), &identity))
      Log("steamworks app=" + std::to_string(record.app_id) + " no download facts: no DLC or depots listed");
    const auto emulator = config_.steamworks_dir.empty() ? config_.runtime_dir + "/steamworks" : config_.steamworks_dir;
    result = rt::EmulateSteamworks(root.string(), emulator, identity);
  } else {
    result = rt::RestoreSteamworks(root.string());
  }
  std::string changed;
  for (const auto& path : result.changed) changed += (changed.empty() ? "" : ",") + path;
  Log("steamworks app=" + std::to_string(record.app_id) + " mode=" + (emulate ? "emulate" : "game") +
      " found=" + std::to_string(result.found.size()) + " changed=" + (changed.empty() ? "-" : changed) +
      (result.ok ? std::string() : " failed=" + result.message));
  if (!result.ok) return result.message;
  // A game from before Steamworks (no steam_api DLL) checks for Steam through steam.dll instead
  // (GUN: "Failed to find Steam."); emulation gives it the minimal one, the game's mode takes it away.
  if (!emulate || result.found.empty()) {
    const auto emulator = config_.steamworks_dir.empty() ? config_.runtime_dir + "/steamworks" : config_.steamworks_dir;
    const auto legacy = emulate ? rt::ProvideLegacySteam(root.string(), emulator, programs)
                                : rt::RemoveLegacySteam(root.string(), programs);
    if (!legacy.changed.empty() || !legacy.ok) {
      std::string added;
      for (const auto& path : legacy.changed) added += (added.empty() ? "" : ",") + path;
      Log("legacy steam app=" + std::to_string(record.app_id) + (emulate ? " added=" : " removed=") + added +
          (legacy.ok ? std::string() : " failed=" + legacy.message));
    }
    if (!legacy.ok) return legacy.message;
  }
  // An Unreal Engine 3 game asks for its EULA through a second program the PS5 cannot start.
  const auto eula = rt::AcceptUe3Eula(root.string(), programs,
                                      (HostPath(record.data_root) / "library/prefixes/game").string(), config_.unix_time());
  if (eula.found)
    Log("ue3 eula app=" + std::to_string(record.app_id) + " guid=" + (eula.guid.empty() ? "-" : eula.guid) +
        " written=" + (eula.written ? "1" : "0") + (eula.ok ? std::string() : " failed=" + eula.message));
  if (!eula.ok) return "The game's EULA could not be marked accepted: " + eula.message + ".";
  // Registry keys Steam's install script writes on a game's first start (Fallout: New Vegas's
  // launcher wants its install path there).
  if (const auto install_dir = rt::ProsperoWindowsPath(record.install_path)) {
    const auto script = rt::ApplyInstallScripts(root.string(), *install_dir,
                                                (HostPath(record.data_root) / "library/prefixes/game").string(),
                                                config_.unix_time());
    if (!script.found.empty()) {
      std::string names;
      for (const auto& name : script.found) names += (names.empty() ? "" : ",") + name;
      Log("install script app=" + std::to_string(record.app_id) + " found=" + names +
          " values=" + std::to_string(script.values) + " skipped=" + std::to_string(script.skipped.size()) +
          (script.ok ? std::string() : " failed=" + script.message));
    }
    if (!script.ok) return "The game's install script could not be applied: " + script.message + ".";
  }
  // Self-contained .NET games reserve more address space than the console grants unless
  // their garbage collector is given a heap limit (runtime/dotnet_heap.h).
  const auto dotnet = rt::LimitDotnetHeap(root.string());
  if (!dotnet.found.empty() || !dotnet.ok) {
    std::string limited;
    for (const auto& path : dotnet.changed) limited += (limited.empty() ? "" : ",") + path;
    Log("dotnet heap app=" + std::to_string(record.app_id) + " found=" + std::to_string(dotnet.found.size()) +
        " changed=" + (limited.empty() ? "-" : limited) + (dotnet.ok ? std::string() : " failed=" + dotnet.message));
  }
  return dotnet.ok ? std::string() : dotnet.message;
}

void RuntimeHost::SetTitleNames(std::map<std::uint32_t, std::string> names) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || names == title_names_) return;
    title_names_ = std::move(names);
    title_names_changed_ = true;
  }
  wake_.notify_all();
}

void RuntimeHost::CheckActivity(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    activity_requests_.push_back(app_id);
  }
  wake_.notify_all();
}

void RuntimeHost::CleanUpRequests() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    cleanup_requested_ = true;
  }
  wake_.notify_all();
}

std::size_t RuntimeHost::LeftoverRequests(Composition& c, bool remove) {
  static constexpr std::string_view prefix = "settings-request-", suffix = ".v1";
  std::set<std::string> referenced;
  for (const auto& record : c.repository.Snapshot())
    if (record.journal) referenced.insert(record.journal->operation_id);
  const auto& directory = c.request_directory;
  std::vector<std::string> leftovers;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
      continue;
    const auto transaction = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (!referenced.count(transaction)) leftovers.push_back(name);
  }
  if (!remove) return leftovers.size();
  z3cloudplay::Storage storage(directory.string());
  std::size_t kept = 0;
  for (const auto& name : leftovers) {
    const auto removed = storage.RemoveDurable(name);
    Log("leftover request " + name + (removed.ok() ? " removed" : " kept: " + removed.message()));
    if (!removed.ok()) ++kept;
  }
  return kept;
}

bool RuntimeHost::Handoff() {
  rt::LaunchResult result;
  std::string install_id;
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!launch_ || !launch_composition_) return false;
    const auto before = launch_->Snapshot();
    if (before.state != rt::LaunchState::kReadyToHandoff) return false;
    install_id = before.install_id;
    result = launch_->Handoff();
    rt::LaunchSnapshot after;
    if (result.accepted) {
      after.install_id = install_id;
      after.session_id = result.session_id;
      after.state = rt::LaunchState::kLaunching;
    } else {
      // A refused handoff leaves the service's own state: still prepared, a
      // recorded failure, or launching when only the last write failed. None
      // of these read wrapper evidence on this thread.
      after = launch_->Snapshot();
    }
    PublishLaunch(*launch_composition_, after);
  }
  Log("play handoff install=" + install_id + " accepted=" + std::to_string(result.accepted) + " session=" +
      result.session_id + " code=" + result.code + " message=" + result.message);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    launch_poll_ = true;
  }
  wake_.notify_all();
  return true;
}

void* RuntimeHost::Entry(void* self) {
  static_cast<RuntimeHost*>(self)->Run();
  return nullptr;
}

void RuntimeHost::Publish(const std::function<void(RuntimeSnapshot&)>& change) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    change(snapshot_);
    ++snapshot_.revision;
  }
  published_.notify_all();
}

rt::NativeActivity RuntimeHost::ActivityNow(std::uint32_t app_id, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return rt::NativeActivity::kUnknown;
  const auto before = snapshot_.activity_serial;
  activity_requests_.push_back(app_id);
  wake_.notify_all();
  const auto answered = [&] {
    const auto found = snapshot_.activity.find(app_id);
    return found != snapshot_.activity.end() && found->second.second > before;
  };
  if (!published_.wait_for(lock, timeout, [&] { return stopping_ || answered(); }) || !answered())
    return rt::NativeActivity::kUnknown;
  return snapshot_.activity.at(app_id).first;
}

void RuntimeHost::ReportWork(std::uint64_t done, std::uint64_t total) {
  if (total == 0) return;
  const int percent = static_cast<int>(std::min<std::uint64_t>(done, total) * 100 / total);
  // Every publish rebuilds the UI's runtime state: only whole-percent changes go out.
  if (percent == work_percent_) return;
  work_percent_ = percent;
  Publish([percent](RuntimeSnapshot& s) { s.work_progress = static_cast<float>(percent) / 100.0F; });
}

void RuntimeHost::ClearWork() {
  if (work_percent_ < 0) return;
  work_percent_ = -1;
  Publish([](RuntimeSnapshot& s) { s.work_progress.reset(); });
}

void RuntimeHost::PublishInstalls(const Composition& composition, const std::string& only) {
  std::map<std::string, bool> checked;
  if (!only.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& view : snapshot_.installs) checked[view.install_id] = view.title_current;
  }
  std::vector<RuntimeInstallView> installs;
  for (const auto& record : composition.repository.Snapshot()) {
    RuntimeInstallView view;
    view.install_id = record.install_id; view.app_id = record.app_id; view.title_id = record.title_id;
    if (!record.data_root.empty()) view.prefix_root = record.data_root + "/library/prefixes/game";
    view.state = record.state;
    if (record.journal) view.step = record.journal->step;
    view.recovery_pending = record.journal.has_value();
    view.awaiting_registration = AwaitingRegistration(record);
    if (record.last_failure) view.failure = record.last_failure->message;
    if (const auto kept = checked.find(record.install_id); !only.empty() && record.install_id != only && kept != checked.end())
      view.title_current = kept->second;
    else if (record.state == rt::PreparationState::kReady && composition.kit)
      view.title_current = composition.installer->TitleCurrent(record) &&
                           composition.installer->TitleMetadataCurrent(record) && TitleNameCurrent(record);
    if (const auto found = composition.admission.find(record.install_id); found != composition.admission.end())
      view.admission = found->second;
    view.settings_revision = record.settings_revision;
    view.launch_entry_id = record.launch_entry_id;
    view.launch_path = record.launch_path;
    view.launch_working_directory = record.launch_working_directory;
    std::error_code error;
    for (std::filesystem::directory_iterator it(SaveBackupRoot(record.install_id), error), end; !error && it != end;
         it.increment(error)) {
      const auto stamp = std::strtoll(it->path().filename().c_str(), nullptr, 10);
      if (stamp <= 0) continue;
      ++view.save_backups;
      view.latest_save_backup = std::max<std::int64_t>(view.latest_save_backup, stamp);
    }
    installs.push_back(std::move(view));
  }
  Publish([&](RuntimeSnapshot& s) { s.installs = std::move(installs); });
}

// The complete OWNER protocol for the publish-profile step: persist the exact
// requested plan, arm OWNER, then capture, publish, register and commit Ready.
// Recovery journals resume their saved request or frozen operation; nothing is
// regenerated from newer settings.
rt::PackageActionResult RuntimeHost::DrivePublication(Composition& c, const rt::RuntimeInstall& record) {
  auto failed = [](std::string code, std::string message) {
    return rt::PackageActionResult{rt::PackageActionState::kFailed, std::move(code), std::move(message)};
  };
  if (!record.journal) return failed("publication-journal", "No preparation journal to publish.");
  const auto& prefix = config_.host_namespace_prefix;
  // Saved request, control setup and frozen publication journals all resume
  // through the persisted request; ResumeRequested hands a frozen publication
  // to Resume itself.
  if (record.journal->operation != "prepare") {
    const auto resumed = c.owner->ResumeRequested(record, c.request_directory, prefix);
    Log("publication resume journal=" + record.journal->operation + "/" + record.journal->step + " state=" +
        std::to_string(static_cast<int>(resumed.state)) + " code=" + resumed.code + " message=" + resumed.message);
    return resumed;
  }
  if (!c.kit) return failed("kit-unavailable", "No verified compatibility kit is installed.");

  const rt::GameRecipe* recipe = nullptr;
  for (const auto& candidate : c.recipes)
    if (candidate.app_id == record.app_id && candidate.id == record.recipe_id) recipe = &candidate;
  if (!recipe) return failed("recipe-unavailable", "Z3CloudPlay no longer knows how to start this game; prepare it again.");
  rt::SettingsLaunchInput input;
  input.recipe = recipe;
  input.record = config_.settings_record ? config_.settings_record(record.app_id).value_or(rt::GameSettingsRecord{})
                                         : rt::GameSettingsRecord{};
  for (const auto& preset : c.presets)
    if (preset.id == input.record.preset_id) input.preset = &preset;
  input.kit = rt::KitCapabilitiesFromManifest(c.kit->capabilities, true);
  input.target = {record.default_profile_id, record.install_path};
  input.runtime_dir = config_.runtime_dir;
  rt::SettingsLaunchPlan plan;
  std::string reason;
  if (input.record.revision != record.settings_revision)
    return failed("settings-changed", "The game's settings changed during preparation; prepare it again.");
  // The first start may come from the PS5 Home screen, so the game's Steam files are set up now.
  if (const auto steamworks = ApplySteamworks(c, record); !steamworks.empty()) return failed("steamworks", steamworks);
  if (!rt::BuildSettingsLaunchPlan(input, &plan, &reason)) {
    Log("publication plan refused: " + reason);
    return failed("settings-plan", reason);
  }
  const auto transaction = config_.uuid();
  if (!transaction) return failed("random-unavailable", "No secure random source for the publication transaction.");
  rt::SettingsPublicationRequest request;
  request.transaction_id = *transaction;
  request.kit_id = c.kit->view.kit_id;
  request.install = record;
  request.plan = std::move(plan);
  request.registry_unix_time = config_.unix_time();
  const auto requested = c.owner->Request(request, c.request_directory);
  Log("publication request transaction=" + request.transaction_id + " state=" +
      std::to_string(static_cast<int>(requested.state)) + " code=" + requested.code + " message=" + requested.message);
  if (requested.state != rt::PackageActionState::kComplete) return requested;
  const auto armed = c.records.Find(record.install_id);
  if (!armed) return failed("publication-owner", "The armed OWNER record is missing.");
  const auto resumed = c.owner->ResumeRequested(*armed, c.request_directory, prefix);
  Log("publication resume state=" + std::to_string(static_cast<int>(resumed.state)) + " code=" + resumed.code +
      " message=" + resumed.message);
  return resumed;
}

// With no working folder set, the profile runs the program from its own folder
// (prospero_profile.cpp), so that is where dxvk.conf goes.
static std::string WorkingFolder(const std::string& working, std::string program) {
  if (!working.empty() && working != ".") return working;
  std::replace(program.begin(), program.end(), '\\', '/');
  const auto slash = program.rfind('/');
  return slash == std::string::npos ? std::string() : program.substr(0, slash);
}

// The folders from the game root down to the program's working folder (where dxvk.conf
// goes) get the same treatment as the root: an owned, world-writable download folder
// becomes 0755. Folders that don't exist yet, links and folders owned by anyone else are
// left alone; publication then refuses them. The caller skips a root on a drive that
// keeps no owners.
std::string RuntimeHost::TightenWorkingFolder(const std::filesystem::path& root, const std::string& working) {
  if (working.empty() || working == ".") return {};
  auto folder = root;
  for (const auto& part : std::filesystem::path(working)) {
    if (part.empty() || part == "." || part == "..") return {};
    folder /= part;
    struct stat info{};
    if (lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid()) return {};
    if ((info.st_mode & 07777) == 0700 || (info.st_mode & 07777) == 0755) continue;
    if (chmod(folder.c_str(), 0755) != 0) return "The game's working folder permissions could not be tightened.";
    char mode[8]; std::snprintf(mode, sizeof(mode), "%04o", unsigned(info.st_mode & 07777));
    Log("game folder mode " + std::string(mode) + " -> 0755 path=" + folder.string());
  }
  return {};
}

std::string RuntimeHost::RepublishSettings(Composition& c, const rt::RuntimeInstall& record, bool* published) {
  if (published) *published = false;
  const auto saved = config_.settings_record ? config_.settings_record(record.app_id).value_or(rt::GameSettingsRecord{})
                                             : rt::GameSettingsRecord{};
  // "Steam features" only swaps the game's own files, so it follows the saved value at once.
  if (saved.revision <= record.settings_revision) return ApplySteamworks(c, record);
  if (!c.kit) return "No verified compatibility kit is installed.";
  const rt::GameRecipe* recipe = nullptr;
  for (const auto& candidate : c.recipes)
    if (candidate.app_id == record.app_id && candidate.id == record.recipe_id &&
        std::to_string(candidate.version) == record.recipe_version)
      recipe = &candidate;
  if (!recipe) return "Z3CloudPlay no longer knows how to start this game; prepare it again.";
  const rt::SettingsPreset* preset = nullptr;
  for (const auto& candidate : c.presets)
    if (candidate.id == saved.preset_id) preset = &candidate;
  const auto kit = rt::KitCapabilitiesFromManifest(c.kit->capabilities, true);
  // Another program or folder chosen in Game settings: the PS5 title and the bottle stay, and
  // the install record names the new program when this publication commits (Reconcile), so a
  // crash in between resumes with it. Callers run this only while the game is stopped.
  auto next = record;
  {
    rt::EffectiveSettings desired;
    rt::SettingsError ignored;
    const auto same = [](std::string left, std::string right) {
      std::replace(left.begin(), left.end(), '\\', '/');
      std::replace(right.begin(), right.end(), '\\', '/');
      return (left == "." ? std::string() : left) == (right == "." ? std::string() : right);
    };
    if (rt::BuildEffectiveSettings(*recipe, preset, saved.overrides, kit, &desired, &ignored) && desired.launch) {
      const auto& launch = *desired.launch;
      if (launch.id != record.launch_entry_id || !same(launch.executable, record.launch_path) ||
          !same(launch.working_directory, record.launch_working_directory)) {
        const auto root = HostPath(record.install_path).string();
        const auto architecture = rt::GameProgramArchitecture(root, launch.executable);
        if (architecture.empty())
          return "The program " + launch.executable + " chosen in Game settings is missing or not a Windows program.";
        if (const auto refusal = rt::GameProgramRefusal(root, launch.executable, kit, rt::RecipeChoicesFrom(saved).try_anyway);
            !refusal.empty())
          return refusal;
        // Checked here, before a publication starts: one that fails part way leaves the title
        // waiting for recovery.
        const auto folder = WorkingFolder(launch.working_directory, launch.executable);
        struct stat folder_info{};
        if (!folder.empty() && (lstat((HostPath(record.install_path) / folder).c_str(), &folder_info) != 0 ||
                                !S_ISDIR(folder_info.st_mode)))
          return "The program folder " + folder + " chosen in Game settings is missing.";
        const auto records_ownership = platform::RecordsOwnership(HostPath(record.install_path));
        if (records_ownership && *records_ownership)
          if (const auto refused = TightenWorkingFolder(HostPath(record.install_path),
                                                        WorkingFolder(launch.working_directory, launch.executable));
              !refused.empty())
            return refused;
        next.launch_entry_id = launch.id;
        next.launch_path = launch.executable;
        next.launch_working_directory = launch.working_directory == "." ? std::string() : launch.working_directory;
        // The recipe stays the one the game was prepared with: its graphics and engine arguments
        // came from the first program's files, and only a preparation sees Steam's launch options.
        Log("program switch app=" + std::to_string(record.app_id) + " from=" + record.launch_entry_id + ":" +
            record.launch_path + " to=" + next.launch_entry_id + ":" + next.launch_path + " recipe kept=" + record.recipe_id);
      }
    }
  }
  // SteamStub unpacking and the Steam emulator follow the program that starts now.
  if (const auto steamworks = ApplySteamworks(c, next); !steamworks.empty()) return steamworks;
  rt::SettingsLaunchInput input;
  input.recipe = recipe;
  input.record = saved;
  // The program and its folder are the install record's; the switch above moved them to the
  // settings' choice.
  auto& overrides = input.record.overrides;
  overrides["launch_option"] = next.launch_entry_id;
  if (next.launch_entry_id == rt::kProgramEntryId) {
    overrides["program"] = next.launch_path;
    // Its architecture comes from the file, whatever the sheet says now.
    const auto architecture = rt::GameProgramArchitecture(HostPath(next.install_path).string(), next.launch_path);
    if (!architecture.empty()) overrides["architecture"] = architecture;
  } else {
    overrides.erase("program");
  }
  overrides["program_folder"] = next.launch_working_directory;
  input.preset = preset;
  input.kit = kit;
  input.target = {next.default_profile_id, next.install_path};
  input.runtime_dir = config_.runtime_dir;
  rt::SettingsLaunchPlan plan;
  std::string reason;
  if (!rt::BuildSettingsLaunchPlan(input, &plan, &reason)) {
    Log("settings update plan refused app=" + std::to_string(record.app_id) + ": " + reason);
    return "The settings could not be applied: " + reason;
  }
  if (plan.app_id != record.app_id || plan.recipe_id != record.recipe_id || plan.recipe_version != record.recipe_version ||
      plan.launch_entry_id != next.launch_entry_id || plan.profile_id != record.default_profile_id)
    return "The settings no longer match this game's PS5 title; prepare it again.";
  const auto transaction = config_.uuid();
  if (!transaction) return "No secure random source for the settings update.";
  rt::SettingsPublicationRequest request;
  request.transaction_id = *transaction;
  request.kit_id = c.kit->view.kit_id;
  request.install = next;
  request.install.settings_revision = saved.revision;  // the revision the update commits
  request.plan = std::move(plan);
  request.registry_unix_time = config_.unix_time();
  const auto requested = c.owner->Request(request, c.request_directory);
  Log("settings update request app=" + std::to_string(record.app_id) + " transaction=" + request.transaction_id +
      " revision=" + std::to_string(record.settings_revision) + "->" + std::to_string(saved.revision) + " state=" +
      std::to_string(static_cast<int>(requested.state)) + " code=" + requested.code + " message=" + requested.message);
  if (requested.state != rt::PackageActionState::kComplete) return "The settings could not be applied: " + requested.message;
  const auto armed = c.records.Find(record.install_id);
  if (!armed) return "The game's install record is missing.";
  const auto resumed = c.owner->ResumeRequested(*armed, c.request_directory, config_.host_namespace_prefix);
  Log("settings update resume app=" + std::to_string(record.app_id) + " state=" +
      std::to_string(static_cast<int>(resumed.state)) + " code=" + resumed.code + " message=" + resumed.message);
  PublishInstalls(c, record.install_id);
  // A failed update keeps its journal, so Play waits until Resume on the PS5 title finishes it.
  if (resumed.state != rt::PackageActionState::kComplete) return "The settings could not be applied: " + resumed.message;
  if (published) *published = true;
  return {};
}

// Fresh facts for one launch, from the package layer, the bottle's start check
// and the console's inventory and activity. Runs on the worker; the application
// thread reuses the worker's latest result while it is fresh.
rt::LaunchPreflight RuntimeHost::Preflight(Composition& c, const rt::RuntimeInstall& record) {
  if (!pthread_equal(pthread_self(), application_thread_)) {
    rt::LaunchPreflight p;
    p.capability_available = c.kit.has_value() && static_cast<bool>(config_.launch_title);
    const auto root = config_.host_namespace_prefix.empty()
        ? std::filesystem::path(record.install_path)
        : config_.host_namespace_prefix / std::filesystem::path(record.install_path).relative_path();
    struct stat info{};
    p.volume_mounted = lstat(root.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    // One full inspection. Its publication check opens the title's publication the way
    // the title's start check does, so when it passes the start check passes; only a
    // refusal asks the diagnostic for its reason.
    const auto package = c.installer->Inspect(record);
    const auto admission = package.profile_published ? std::string("ok") : c.installer->AdmissionDiagnostic(record);
    c.admission[record.install_id] = admission;
    p.bottle_present = package.mutable_bottle_present && package.mutable_bottle_owned && admission == "ok";
    p.runtime_present = package.artifacts_valid;
    p.helper_valid = package.title_staged && package.manifest_matches && c.installer->TitleCurrent(record);
    p.profile_present = package.profile_published;
    p.settings_revision = package.profile_settings_revision;
    // The title's files and runtime were just checked above; the console's lists only
    // need to name this install's title (no second full inspection).
    const auto native = c.coordinator->QueryOwnership(record);
    p.title_owned = native.installed && native.owned_manifest_matches && !native.collision;
    p.inventory_confirmed = native.available && !native.scan_pending;
    p.activity = c.coordinator->Activity(record.title_id);
    c.preflights[record.install_id] = {p, config_.now_ms()};
    Log("play preflight install=" + record.install_id + " capability=" + std::to_string(p.capability_available) +
        " volume=" + std::to_string(p.volume_mounted) + " bottle=" + std::to_string(p.bottle_present) +
        " admission=" + admission + " runtime=" + std::to_string(p.runtime_present) +
        " helper=" + std::to_string(p.helper_valid) + " profile=" + std::to_string(p.profile_present) +
        " title=" + std::to_string(p.title_owned) + " inventory=" + std::to_string(p.inventory_confirmed) +
        " activity=" + std::to_string(static_cast<int>(p.activity)) + " revision=" + std::to_string(p.settings_revision));
    return p;
  }
  const auto cached = c.preflights.find(record.install_id);
  if (cached == c.preflights.end() || config_.now_ms() - cached->second.second > kPreflightFreshMs ||
      config_.now_ms() < cached->second.second)
    return {};
  return cached->second.first;
}

rt::LaunchEvidence RuntimeHost::Evidence(Composition& c, const rt::RuntimeInstall& record,
                                         const std::string& session_id) {
  rt::LaunchEvidence evidence;
  evidence.state = rt::LaunchEvidenceState::kNone;
  const auto listed = c.sessions->Read(record, session_id);
  if (!listed.ok) {
    Log("play evidence install=" + record.install_id + " unavailable code=" + listed.code + " message=" + listed.message);
    return evidence;
  }
  for (const auto& session : listed.sessions) {
    if (session.session_id != session_id) continue;
    evidence.session_id = session.session_id;
    evidence.install_id = session.install_id;
    evidence.title_id = session.title_id;
    evidence.runtime_bundle_id = session.runtime_bundle_id;
    evidence.settings_revision = session.settings_revision;
    evidence.exit_code = session.exit_code;
    // A Z3CloudPlay handoff must come back as a Z3CloudPlay session.
    evidence.valid = session.origin == rt::SessionOrigin::kZ3CloudPlay;
    switch (session.phase) {
      case rt::WrapperSessionPhase::kStarting: evidence.state = rt::LaunchEvidenceState::kStarting; break;
      case rt::WrapperSessionPhase::kRunning: evidence.state = rt::LaunchEvidenceState::kRunning; break;
      case rt::WrapperSessionPhase::kExited: evidence.state = rt::LaunchEvidenceState::kExited; break;
      case rt::WrapperSessionPhase::kFailed:
        evidence.state = rt::LaunchEvidenceState::kFailed;
        evidence.code = "wrapper-start-failed";
        evidence.message = "The game did not start.";
        break;
    }
  }
  return evidence;
}

void RuntimeHost::PublishLaunch(Composition& c, const rt::LaunchSnapshot& launch) {
  RuntimeLaunchView view;
  view.install_id = launch.install_id;
  view.state = launch.state;
  view.code = launch.code;
  view.message = launch.message;
  view.exit_code = launch.exit_code;
  if (const auto record = c.records.Find(launch.install_id)) {
    view.app_id = record->app_id;
    view.title_id = record->title_id;
  }
  Publish([&](RuntimeSnapshot& s) {
    s.handoff_ready = launch.state == rt::LaunchState::kReadyToHandoff;
    if (launch.state == rt::LaunchState::kIdle && launch.install_id.empty()) s.launch.reset();
    else s.launch = view;
  });
}

void RuntimeHost::RunPlay(Composition& c, std::uint32_t app_id) {
  std::optional<rt::RuntimeInstall> record;
  for (const auto& candidate : c.repository.Snapshot())
    if (candidate.app_id == app_id) record = candidate;
  rt::LaunchResult result;
  rt::LaunchSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!c.launch) {
      result = rt::LaunchResult::Failure(rt::LaunchError::kPersistence, "Play is unavailable.");
    } else if (!record) {
      result = rt::LaunchResult::Failure(rt::LaunchError::kNotFound, "This game has no PS5 title yet.");
    } else {
      // Settings changed since the title's publication take effect now, while nothing of
      // the game runs: Home and Z3CloudPlay then start the same published profile.
      bool idle = false;
      if (c.launch_journal) {
        const auto pending = c.launch_journal->LoadPending();
        idle = pending.ok && !pending.session && !record->journal &&
               record->state == rt::PreparationState::kReady &&
               c.coordinator->Activity(record->title_id) == rt::NativeActivity::kStopped;
      }
      const auto settings = idle ? RepublishSettings(c, *record) : std::string();
      result = settings.empty() ? c.launch->Request(record->install_id)
                                : rt::LaunchResult::Failure(rt::LaunchError::kPreflight, settings);
    }
    if (result.accepted) {
      snapshot = c.launch->Snapshot();
      PublishLaunch(c, snapshot);
    }
  }
  // A refused request leaves any pending session as it is; the refusal is
  // shown for this game only.
  Publish([&](RuntimeSnapshot& s) {
    ++s.plays_handled;
    if (result.accepted) s.play_refusals.erase(app_id);
    else s.play_refusals[app_id] = result.message.empty() ? "The game could not start." : result.message;
  });
  Log("play request app=" + std::to_string(app_id) + " accepted=" + std::to_string(result.accepted) +
      " session=" + result.session_id + " code=" + result.code + " message=" + result.message);
  PublishInstalls(c);
}

bool RuntimeHost::PollLaunch(Composition& c) {
  std::lock_guard<std::mutex> lock(launch_mutex_);
  if (!c.launch) return false;
  auto snapshot = c.launch->Snapshot();
  if (snapshot.state == rt::LaunchState::kReadyToHandoff) {
    PublishLaunch(c, snapshot);
    return true;
  }
  const bool pending = c.launch_journal && [&] {
    const auto loaded = c.launch_journal->LoadPending();
    return loaded.ok && loaded.session.has_value();
  }();
  if (pending && snapshot.state == rt::LaunchState::kFailed) {
    // A failure read from bad or missing evidence says nothing about the title
    // itself: end the session only once the console shows the title stopped.
    const auto record = c.records.Find(snapshot.install_id);
    if (record && c.coordinator->Activity(record->title_id) != rt::NativeActivity::kStopped) {
      PublishLaunch(c, snapshot);
      return true;
    }
  }
  if (pending && (snapshot.state == rt::LaunchState::kExited || snapshot.state == rt::LaunchState::kFailed)) {
    const auto finished = c.launch->Finish();
    Log("play finished install=" + snapshot.install_id + " session=" + snapshot.session_id + " state=" +
        std::to_string(static_cast<int>(snapshot.state)) + " code=" + snapshot.code + " exit=" +
        (snapshot.exit_code ? std::to_string(*snapshot.exit_code) : std::string("unknown")) +
        " cleared=" + std::to_string(finished.accepted) + " " + finished.code);
    PublishLaunch(c, snapshot);
    return !finished.accepted;
  }
  PublishLaunch(c, snapshot);
  return pending;
}

namespace {
// JSON string body for a title name: quotes, backslashes and control characters escaped.
std::string JsonEscaped(const std::string& text) {
  std::string out;
  for (const unsigned char ch : text) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
    else if (ch < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", ch); out += code; }
    else out += static_cast<char>(ch);
  }
  return out;
}
}  // namespace

namespace {
// The "titleName" value of a param.json, or nothing.
std::optional<std::string> TitleNameOf(const std::string& param) {
  static constexpr std::string_view key = "\"titleName\"";
  const auto at = param.find(key);
  if (at == std::string::npos) return std::nullopt;
  auto cursor = param.find_first_not_of(" \t\r\n", at + key.size());
  if (cursor == std::string::npos || param[cursor] != ':') return std::nullopt;
  cursor = param.find_first_not_of(" \t\r\n", cursor + 1);
  if (cursor == std::string::npos || param[cursor] != '"') return std::nullopt;
  std::string value;
  for (auto i = cursor + 1; i < param.size(); ++i) {
    if (param[i] == '"') return value;
    if (param[i] == '\\' && i + 1 < param.size()) ++i;
    value += param[i];
  }
  return std::nullopt;
}
}  // namespace

std::optional<std::string> RuntimeHost::InstalledTitleName(const rt::RuntimeInstall& record) const {
  if (record.data_root.empty()) return std::nullopt;
  const auto root = config_.host_namespace_prefix.empty()
      ? std::filesystem::path(record.data_root)
      : config_.host_namespace_prefix / std::filesystem::path(record.data_root).relative_path();
  const auto path = root / "title-metadata/sce_sys/param.json";
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string bytes;
  char chunk[4096];
  for (ssize_t count; (count = read(fd, chunk, sizeof(chunk))) > 0 && bytes.size() < 64 * 1024;)
    bytes.append(chunk, static_cast<std::size_t>(count));
  close(fd);
  return TitleNameOf(bytes);
}

bool RuntimeHost::TitleNameCurrent(const rt::RuntimeInstall& record) const {
  std::string wanted;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = title_names_.find(record.app_id);
    if (found == title_names_.end() || found->second.empty()) return true;  // nothing asked for
    wanted = found->second;
  }
  const auto installed = InstalledTitleName(record);
  return installed && *installed == wanted;
}

rt::PackageActionResult RuntimeHost::WriteTitleMetadata(Composition& c, const rt::RuntimeInstall& record,
                                                       const std::string& title_name) {
  const auto failed = [](std::string message) {
    return rt::PackageActionResult{rt::PackageActionState::kFailed, "title-metadata", std::move(message)};
  };
  if (!c.kit) return failed("No verified compatibility kit is installed.");
  const rt::KitTitleVariant* variant = rt::KitVariantFor(c.kit->view, record.title_id);
  if (!variant) return failed("The kit has no title for this game.");
  // The kit's param.json with the game's name in place of the kit's.
  std::string param;
  {
    const auto path = c.kit->view.root / variant->path / "sce_sys/param.json";
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return failed("The kit's title parameters are unreadable.");
    char chunk[4096];
    for (ssize_t count; (count = read(fd, chunk, sizeof(chunk))) > 0 && param.size() < 64 * 1024;)
      param.append(chunk, static_cast<std::size_t>(count));
    close(fd);
  }
  // Replaces the value of the single "<key>": "<value>" entry; false for any other layout.
  const auto replace_value = [&param](std::string_view key, const std::string& value) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    const auto at = param.find(quoted);
    if (at == std::string::npos || param.find(quoted, at + quoted.size()) != std::string::npos) return false;
    auto cursor = param.find_first_not_of(" \t\r\n", at + quoted.size());
    if (cursor == std::string::npos || param[cursor] != ':') return false;
    cursor = param.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor == std::string::npos || param[cursor] != '"') return false;
    const auto end = param.find('"', cursor + 1);
    if (end == std::string::npos) return false;
    param.replace(cursor + 1, end - cursor - 1, value);
    return true;
  };
  // The any-title shell is one title for every game: each copy gets its own title ID.
  if (variant->any_title &&
      !(replace_value("titleId", record.title_id) && replace_value("conceptId", record.title_id.substr(4)) &&
        replace_value("contentId", "UP9000-" + record.title_id + "_00-PROSPEROWIN00001")))
    return failed("The kit's title parameters have an unexpected layout.");
  const std::string name = title_name.empty() ? "Steam game " + std::to_string(record.app_id) : title_name;
  if (!replace_value("titleName", JsonEscaped(name)))
    Log("title metadata app=" + std::to_string(record.app_id) + " keeps the kit's name: unexpected layout");
  const auto icon = config_.title_icon ? config_.title_icon(record.app_id) : std::nullopt;
  Log("title metadata app=" + std::to_string(record.app_id) + " name=" + name + " icon=" +
      (icon ? std::to_string(icon->size()) + "B" : std::string("kit")));
  return c.installer->WriteTitleMetadata(record, param, icon);
}

namespace {
// Generated recipes live in the private runtime state, one file per game.
std::string GeneratedRecipeFile(const std::string& recipe_id) { return "recipe-" + recipe_id + ".json"; }
}  // namespace

// The recipe a game is prepared with: Z3CloudPlay's shipped one, otherwise one generated
// from Steam's launch options and saved so recovery and later settings changes use the
// same launch entries. Returns null with the reason for the player.
const rt::GameRecipe* RuntimeHost::RecipeForPrepare(Composition& c, const RuntimePrepareRequest& request,
                                                    std::string* reason) {
  for (const auto& candidate : c.recipes)
    if (candidate.app_id == request.app_id && !rt::IsGeneratedRecipeId(candidate.id)) return &candidate;
  if (!c.settings_data_reason.empty()) { *reason = c.settings_data_reason; return nullptr; }
  const auto root = config_.host_namespace_prefix.empty()
      ? std::filesystem::path(request.install_path)
      : config_.host_namespace_prefix / std::filesystem::path(request.install_path).relative_path();
  // The program the player chose and "Try anyway" come from Game settings, so a game whose
  // Steam launch options don't work can still be started.
  const auto saved = config_.settings_record ? config_.settings_record(request.app_id) : std::nullopt;
  const auto choices = saved ? rt::RecipeChoicesFrom(*saved) : rt::RecipeChoices{};
  const auto kit = c.kit ? rt::KitCapabilitiesFromManifest(c.kit->capabilities, true) : rt::KitCapabilities{};
  rt::GameRecipe generated;
  bool soft = true;
  rt::GameRecipeDiagnostics diagnostics;
  const bool made = (!request.launches.empty() || !choices.program.empty()) &&
      rt::GenerateGameRecipe(request.app_id, request.title_name, request.launches, root.string(), &generated, reason, kit,
                             choices, &soft, &diagnostics);
  // The same line an unpack writes, so a refused SteamStub program is found the same way.
  if (!made && !diagnostics.steam_stub_program.empty())
    Log("steamstub app=" + std::to_string(request.app_id) + " found=" + diagnostics.steam_stub_program +
        " changed=- failed=" + *reason + " (" + (diagnostics.steam_stub_detail.empty() ? "unknown" : diagnostics.steam_stub_detail) + ")");
  if (!made && request.launches.empty() && choices.program.empty())
    *reason = request.launch_reason.empty() ? "Steam lists no Windows program this console can start for this game."
                                            : request.launch_reason;
  // A refused game gets the folder's programs and a draft for Game settings to start from; a
  // prepared one is scanned when its Program picker opens (ScanPrograms).
  std::vector<rt::GameProgram> programs;
  std::optional<rt::GameRecipe> draft;
  if (!made) {
    programs = rt::ListGamePrograms(root.string(), kit);
    auto tried = choices;
    tried.try_anyway = true;
    rt::GameRecipe permissive;
    std::string ignored;
    if (!rt::GenerateGameRecipe(request.app_id, request.title_name, request.launches, root.string(), &permissive,
                                &ignored, kit, tried)) {
      // No program yet: the sheet starts from the Program row.
      permissive = {};
      permissive.id = rt::GeneratedRecipeId(request.app_id);
      permissive.version = 2;
      permissive.app_id = request.app_id;
      permissive.name = request.title_name;
      permissive.defaults["steam_features"] = "emulate";
    }
    draft = std::move(permissive);
    Log("prepare refused app=" + std::to_string(request.app_id) + " soft=" + std::to_string(soft) +
        " programs=" + std::to_string(programs.size()) + " " + *reason);
  }
  Publish([&](RuntimeSnapshot& s) {
    auto& setup = s.game_setups[request.app_id];
    if (!made) setup.programs = std::move(programs), setup.scanned = true;
    setup.draft = std::move(draft);
    setup.refusal_soft = !made && soft;
  });
  if (!made) return nullptr;
  const auto text = rt::SerializeGameRecipe(generated);
  const auto written = c.storage.WriteAtomicPrivate(GeneratedRecipeFile(generated.id), Bytes(text.begin(), text.end()));
  if (!written.ok()) {
    Log("generated recipe write failed app=" + std::to_string(request.app_id) + " message=" + written.message());
    *reason = "Z3CloudPlay could not save how to start this game.";
    return nullptr;
  }
  Log("generated recipe app=" + std::to_string(request.app_id) + " detected=" + generated.detected +
      " architecture=" + generated.architecture +
      " entries=" + std::to_string(generated.launch_entries.size()) + " executable=" +
      generated.launch_entries.front().executable);
  c.recipes.erase(std::remove_if(c.recipes.begin(), c.recipes.end(),
                                 [&](const rt::GameRecipe& recipe) { return recipe.id == generated.id; }),
                  c.recipes.end());
  c.recipes.push_back(std::move(generated));
  PublishRecipes(c);
  return &c.recipes.back();
}

void RuntimeHost::LoadGeneratedRecipes(Composition& c) {
  for (const auto& record : c.repository.Snapshot()) {
    if (!rt::IsGeneratedRecipeId(record.recipe_id)) continue;
    const auto bytes = c.storage.Read(GeneratedRecipeFile(record.recipe_id), 256 * 1024);
    rt::GameRecipe recipe;
    std::string reason;
    if (!bytes.ok() ||
        !rt::ParseGameRecipe(std::string(bytes.value().begin(), bytes.value().end()), &recipe, &reason) ||
        recipe.id != record.recipe_id || recipe.app_id != record.app_id ||
        std::to_string(recipe.version) != record.recipe_version) {
      Log("generated recipe unavailable install=" + record.install_id + " " +
          (bytes.ok() ? reason : bytes.message()));
      continue;
    }
    c.recipes.push_back(std::move(recipe));
  }
  PublishRecipes(c);
}

void RuntimeHost::PublishRecipes(const Composition& c) {
  std::vector<rt::GameRecipe> generated;
  for (const auto& recipe : c.recipes)
    if (rt::IsGeneratedRecipeId(recipe.id)) generated.push_back(recipe);
  Publish([&](RuntimeSnapshot& s) { s.generated_recipes = std::move(generated); });
}

std::string RuntimeHost::RunPrepare(Composition& c, const RuntimePrepareRequest& request, bool* awaiting) {
  if (!c.kit) return Snapshot().kit_reason;
  // Where the time before the first step goes: the recipe, the reservation and the title's metadata.
  const auto began = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&began] {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count());
  };
  // A game being started or played keeps its title and bottle as they are.
  if (c.launch_journal) {
    const auto pending = c.launch_journal->LoadPending();
    if (!pending.ok) return "The launch record cannot be read; restart Z3CloudPlay before preparing.";
    if (pending.session && pending.session->install_id == InstallId(request.app_id))
      return "Close the game before updating or repairing its PS5 title.";
  }
  std::optional<rt::RuntimeInstall> existing;
  for (const auto& record : c.repository.Snapshot())
    if (record.app_id == request.app_id) existing = record;
  if (existing && existing->state == rt::PreparationState::kRemoving)
    return "This game's PS5 title is still being removed. Uninstall the game again to finish.";
  rt::InstallServiceResult started = rt::InstallServiceResult::Success();
  bool game_root_owned = true;
  const auto tighten_working = [&](const std::string& working) -> std::string {
    return game_root_owned ? TightenWorkingFolder(HostPath(request.install_path), working) : std::string();
  };
  // Settings publication writes dxvk.conf into the game folder and requires that
  // folder to belong to the app user without group/other write access. Console
  // downloads are created world-writable; tighten an owned root to 0755. A root
  // owned by anyone else is refused, never taken over. A drive that records no
  // owners or permissions (exFAT) has nothing to check or tighten; the kit must
  // then admit such folders (publication metadata schema 3).
  {
    const auto root = config_.host_namespace_prefix.empty()
        ? std::filesystem::path(request.install_path)
        : config_.host_namespace_prefix / std::filesystem::path(request.install_path).relative_path();
    struct stat info{};
    if (lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
      return "The game's folder is unavailable.";
    const auto records_ownership = platform::RecordsOwnership(root);
    if (!records_ownership) return "The game's folder can't be written.";
    if (!*records_ownership) {
      if (!rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).game_root_unowned)
        return "This drive doesn't keep file owners, and the installed runtime kit can't start games from it. "
               "Install the game on console storage, or update the kit.";
      Log("game root without ownership path=" + request.install_path);
      game_root_owned = false;
    } else if (info.st_uid != geteuid()) {
      return "The game's folder belongs to another user.";
    } else if ((info.st_mode & 07777) != 0700 && (info.st_mode & 07777) != 0755) {
      if (chmod(root.c_str(), 0755) != 0) return "The game's folder permissions could not be tightened.";
      char mode[8]; std::snprintf(mode, sizeof(mode), "%04o", unsigned(info.st_mode & 07777));
      Log("game root mode " + std::string(mode) + " -> 0755 path=" + request.install_path);
    }
  }
  if (existing) {
    if (const auto refused = tighten_working(WorkingFolder(existing->launch_working_directory, existing->launch_path));
        !refused.empty())
      return refused;
  }
  // Reservations survive a failed allow-list or metadata write. Complete these
  // prerequisites again before a fresh start, keeping already-bound artifacts.
  const auto initialize_reserved = [&](rt::RuntimeInstall& record) -> std::string {
    if (record.artifacts.empty()) {
      for (const auto& artifact : c.kit->view.artifacts)
        record.artifacts.push_back({artifact.artifact_id, artifact.identity, artifact.sha256});
      const auto bound = c.repository.Update(record);
      if (!bound.ok()) return "The kit artifacts could not be recorded: " + bound.message();
    }
    if (!c.installer->TitleMetadataCurrent(record)) {
      const auto written = WriteTitleMetadata(c, record, request.title_name);
      if (written.state != rt::PackageActionState::kComplete) return written.message;
    }
    return {};
  };
  if (existing) {
    // Startup may have failed to publish the list. Never install a rule shell
    // until its durable reservation is admitted by the helper.
    if (const auto listed = SyncTitleAllowList(c); !listed.empty()) return listed;
    if (existing->state == rt::PreparationState::kReady) {
      if (c.coordinator->Activity(existing->title_id) != rt::NativeActivity::kStopped)
        return "Close the game before updating or repairing its PS5 title.";
      // An interrupted settings update resumes its saved request; nothing is regenerated.
      if (existing->journal) {
        const auto resumed = DrivePublication(c, *existing);
        PublishInstalls(c);
        if (resumed.state != rt::PackageActionState::kComplete) return resumed.message;
        return {};
      }
      bool changed = false;
      // Older titles carry the kit's generic name and icon: give them the game's.
      const bool metadata_current = c.installer->TitleMetadataCurrent(*existing) &&
                                    (request.title_name.empty() || InstalledTitleName(*existing) == request.title_name);
      if (!metadata_current) {
        const auto written = WriteTitleMetadata(c, *existing, request.title_name);
        if (written.state != rt::PackageActionState::kComplete) return written.message;
      }
      if (!c.installer->TitleCurrent(*existing) || !metadata_current) {
        // Same kit id and recipe; title files changed (a new wrapper build) or
        // the runtime was rebuilt (same artifacts, new bytes). The registration
        // and publication stay as they are. A rebuilt runtime is copied into the
        // title first and its new artifact hashes recorded only after that.
        auto target = *existing;
        bool runtime_upgrade = false;
        if (target.artifacts.size() == c.kit->view.artifacts.size()) {
          std::vector<rt::RuntimeArtifactIdentity> upgraded;
          bool same_set = true;
          for (const auto& artifact : c.kit->view.artifacts) {
            bool found = false;
            for (const auto& bound : target.artifacts)
              if (bound.artifact_id == artifact.artifact_id) {
                found = true;
                // The recipe must be the one this install was prepared from.
                if (artifact.artifact_id != "runtime" && bound.sha256 != artifact.sha256) same_set = false;
              }
            same_set = same_set && found;
            upgraded.push_back({artifact.artifact_id, artifact.identity, artifact.sha256});
          }
          const auto differs = [&] {
            for (const auto& wanted : upgraded) {
              bool equal = false;
              for (const auto& bound : target.artifacts)
                equal |= bound.artifact_id == wanted.artifact_id && bound.identity == wanted.identity &&
                         bound.sha256 == wanted.sha256;
              if (!equal) return true;
            }
            return false;
          };
          if (same_set && differs()) {
            target.artifacts = std::move(upgraded);
            runtime_upgrade = true;
          }
        }
        const auto refreshed = c.installer->RefreshTitle(target);
        if (runtime_upgrade && refreshed.state == rt::PackageActionState::kComplete) {
          const auto recorded = c.repository.Update(target);
          Log("runtime upgrade app=" + std::to_string(request.app_id) + " recorded=" + std::to_string(recorded.ok()));
          if (!recorded.ok()) return "The runtime was updated but could not be recorded; press Update again.";
          *existing = target;
        }
        Log("title refresh app=" + std::to_string(request.app_id) + " state=" +
            std::to_string(static_cast<int>(refreshed.state)) + " code=" + refreshed.code + " message=" + refreshed.message);
        PublishInstalls(c);
        if (refreshed.state != rt::PackageActionState::kComplete) return refreshed.message;
        changed = true;
      }
      auto admission = c.installer->AdmissionDiagnostic(*existing);
      if (admission != "ok") {
        const auto repaired = c.installer->RepairBottle(*existing);
        admission = c.installer->AdmissionDiagnostic(*existing);
        Log("bottle repair app=" + std::to_string(request.app_id) + " state=" +
            std::to_string(static_cast<int>(repaired.state)) + " code=" + repaired.code + " admission=" + admission);
        if (repaired.state != rt::PackageActionState::kComplete) {
          c.admission[existing->install_id] = admission; PublishInstalls(c);
          return repaired.message;
        }
        changed = true;
      }
      c.admission[existing->install_id] = admission;
      PublishInstalls(c);
      if (admission != "ok") return "The PS5 title still fails its start check: " + admission;
      return changed ? std::string{} : "This game is already prepared.";
    }
    if (existing->install_path != request.install_path)
      return "The game's files moved since preparation started; recovery is not connected yet.";
    // A generated recipe that could not be read back is made again before a fresh start.
    const bool fresh_start = existing->state == rt::PreparationState::kFailed ||
                             existing->state == rt::PreparationState::kUnavailable ||
                             existing->state == rt::PreparationState::kDownloaded;
    if (fresh_start && rt::IsGeneratedRecipeId(existing->recipe_id) &&
        std::none_of(c.recipes.begin(), c.recipes.end(),
                     [&](const rt::GameRecipe& recipe) { return recipe.id == existing->recipe_id; })) {
      std::string reason;
      const auto* recipe = RecipeForPrepare(c, request, &reason);
      if (!recipe) return reason;
      if (recipe->id != existing->recipe_id || std::to_string(recipe->version) != existing->recipe_version)
        return "This game's setup changed; remove its PS5 title before preparing it again.";
    }
    if (fresh_start)
      if (const auto initialized = initialize_reserved(*existing); !initialized.empty()) return initialized;
    if (existing->state == rt::PreparationState::kFailed || existing->state == rt::PreparationState::kUnavailable)
      started = c.service->Retry(existing->install_id);
    else if (existing->state == rt::PreparationState::kDownloaded)
      started = c.service->Prepare(existing->install_id);
    else if (!existing->journal)
      return "The preparation record has no journal to continue.";
    // Re-ensure the staged title (idempotent for a matching title; it also
    // repairs directory modes from earlier builds) before resuming.
    if (existing->state == rt::PreparationState::kPreparing || existing->state == rt::PreparationState::kRegistering) {
      const auto staged = c.installer->StageTitle(*existing);
      Log("resume title check state=" + std::to_string(static_cast<int>(staged.state)) + " code=" + staged.code +
          " message=" + staged.message);
    }
    // Otherwise this is an explicit resume of the saved journal: the service
    // routes it to the saved request or frozen operation, never a new plan, and
    // never resubmits an uncertain registration.
  } else {
    std::string reason;
    const rt::GameRecipe* recipe = RecipeForPrepare(c, request, &reason);
    if (!recipe) return reason;
    // A shipped recipe is trusted only from the kit that carries it.
    if (!rt::IsGeneratedRecipeId(recipe->id) &&
        (recipe->id != c.kit->view.recipe.id || std::to_string(recipe->version) != c.kit->view.recipe.version))
      return "The installed compatibility kit was built for another version of this game's setup.";
    if (recipe->launch_entries.empty()) return "Steam lists no Windows program this console can start for this game.";
    const auto settings = config_.settings_record ? config_.settings_record(request.app_id) : std::nullopt;
    // The title is made for the launch option the player chose, so its publication matches.
    rt::LaunchEntry launch = recipe->launch_entries.front();
    if (settings) {
      const rt::SettingsPreset* preset = nullptr;
      for (const auto& candidate : c.presets)
        if (candidate.id == settings->preset_id) preset = &candidate;
      rt::EffectiveSettings effective;
      rt::SettingsError ignored;
      if (rt::BuildEffectiveSettings(*recipe, preset, settings->overrides, rt::KitCapabilitiesFromManifest(c.kit->capabilities, true),
                                     &effective, &ignored) && effective.launch)
        launch = *effective.launch;
      // A program the player picked is checked on disk: it must be there, built as the sheet says.
      if (effective.launch && effective.launch->id == rt::kProgramEntryId) {
        const auto root = HostPath(request.install_path).string();
        const auto found = rt::GameProgramArchitecture(root, launch.executable);
        if (found.empty()) return "The program " + launch.executable + " chosen in Game settings is missing or not a Windows program.";
        if (found != effective.architecture)
          return "The program " + launch.executable + " is " + (found == "pe64" ? "64-bit" : "32-bit") +
                 " but Game settings says otherwise; choose it again in Game settings.";
        // "Don't start" holds for the picked program too, even when another launch option made the recipe.
        const auto refusal = rt::GameProgramRefusal(root, launch.executable, rt::KitCapabilitiesFromManifest(c.kit->capabilities, true),
                                                    rt::RecipeChoicesFrom(*settings).try_anyway);
        if (!refusal.empty()) return refusal;
      }
    }
    if (const auto refused = tighten_working(WorkingFolder(launch.working_directory, launch.executable)); !refused.empty())
      return refused;
    rt::ReserveRequest reserve;
    reserve.install_id = InstallId(request.app_id);
    reserve.app_id = request.app_id;
    reserve.default_profile_id = rt::ProsperoProfileId(request.app_id);
    reserve.runtime_bundle_id = c.kit->view.runtime.bundle_id;
    reserve.recipe_id = recipe->id;
    reserve.recipe_version = std::to_string(recipe->version);
    reserve.settings_revision = settings ? settings->revision : 0;
    reserve.launch_entry_id = launch.id;
    reserve.launch_path = launch.executable;
    reserve.launch_working_directory = launch.working_directory;
    reserve.install_path = request.install_path;
    reserve.volume_id = request.volume_id;
    // Titles removed during this run go last: a kit with fixed titles still gets one back.
    std::vector<std::string> retired;
    for (auto& title : rt::KitTitleCandidates(c.kit->view))
      (c.retired_titles.count(title) ? retired : reserve.kit_title_variants).push_back(std::move(title));
    reserve.kit_title_variants.insert(reserve.kit_title_variants.end(), retired.begin(), retired.end());
    auto reserved = c.repository.Reserve(reserve);
    if (!reserved.ok()) return "A PS5 title could not be reserved: " + reserved.message();
    auto record = reserved.value();
    // Before the title exists: it may only start once its helper lists it.
    Log("prepare reserved app=" + std::to_string(request.app_id) + " title=" + record.title_id + " ms=" + elapsed_ms());
    if (const auto listed = SyncTitleAllowList(c); !listed.empty()) return listed;
    if (const auto initialized = initialize_reserved(record); !initialized.empty()) return initialized;
    Log("prepare metadata app=" + std::to_string(request.app_id) + " ms=" + elapsed_ms());
    PublishInstalls(c);
    started = c.service->Prepare(record.install_id);
  }
  PublishInstalls(c);
  Log("prepare start app=" + std::to_string(request.app_id) + " root=" + request.install_path +
      " ok=" + std::to_string(started.ok()) + " ms=" + elapsed_ms() + " message=" + started.message);
  if (!started.ok()) return started.message;
  const auto install_id = existing ? existing->install_id : InstallId(request.app_id);
  bool checked_after_submit = false;
  for (int steps = 0; steps < kMaxPreparationSteps && !Stopping(); ++steps) {
    const auto current = c.records.Find(install_id);
    if (!current) return "The preparation record disappeared.";
    if (current->state == rt::PreparationState::kReady) return {};
    if (current->state != rt::PreparationState::kPreparing && current->state != rt::PreparationState::kRegistering)
      return current->last_failure ? current->last_failure->message : "Preparation stopped.";
    const auto before = current->journal ? current->journal->step : std::string{};
    const auto step_began = std::chrono::steady_clock::now();
    const auto step = c.service->Step(install_id);
    const auto step_ended = std::chrono::steady_clock::now();
    ClearWork();  // a step's progress ends with it
    PublishInstalls(c);
    const auto ms = [](auto from, auto to) {
      return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    const auto after_step = c.records.Find(install_id);
    Log("prepare step app=" + std::to_string(request.app_id) + " from=" + before +
        " step_ms=" + ms(step_began, step_ended) + " publish_ms=" + ms(step_ended, std::chrono::steady_clock::now()) +
        " ok=" + std::to_string(step.ok()) + " error=" + std::to_string(static_cast<int>(step.error())) +
        " state=" + (after_step ? std::to_string(static_cast<int>(after_step->state)) : std::string("-")) +
        " to=" + (after_step && after_step->journal ? after_step->journal->step : std::string{}) +
        " message=" + step.message);
    // A submitted registration is not polled: ShadowMountPlus registers titles only
    // while no title runs, Z3CloudPlay included. The console is asked once more right
    // away (a title may already be registered); otherwise preparation ends here and
    // the game is ready to play once the player leaves Z3CloudPlay. Later checks only
    // query the console and never resubmit an armed registration.
    const bool pending = !step.ok() && step.error() == rt::InstallServiceError::kRegistrationPending;
    if (!step.ok() && !pending) return step.message;
    const auto after = c.records.Find(install_id);
    if (pending || (after && after->journal && after->journal->step == "await-native")) {
      if (pending || checked_after_submit) {
        if (awaiting) *awaiting = true;
        return {};
      }
      checked_after_submit = true;
    }
  }
  return Stopping() ? "Preparation was interrupted." : "Preparation did not finish.";
}

namespace {
// Kept bottles: <games>/<install id>.kept, identified by this marker inside.
constexpr char kKeptMarker[] = "z3cloudplay-kept.json";
constexpr char kKeptSuffix[] = ".kept";
// Save backups: <games>/<install id>.saves/<Unix time>/users, the newest few kept.
constexpr char kSaveBackupSuffix[] = ".saves";
constexpr std::size_t kSaveBackupsKept = 3;

bool SteamInstallId(const std::string& id) {
  if (id.rfind("steam-", 0) != 0 || id.size() <= 6 || id.size() > 16) return false;
  return id.find_first_not_of("0123456789", 6) == std::string::npos;
}

// Deletes a tree with lstat/unlink/rmdir: links are removed as links and never
// followed (Wine prefixes link z: to /). std::filesystem::remove_all crashed the
// console title on a retired title folder, so it is not used here. A missing root
// is not an error; the first failure is reported and the walk continues.
// `removed`, when given, is called after each entry the walk deletes (or fails to).
void RemoveTreeNoFollow(const std::string& path, std::error_code& error,
                        const std::function<void()>* removed = nullptr) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) {
    if (errno != ENOENT && !error) error = std::error_code(errno, std::generic_category());
    return;
  }
  if (S_ISDIR(info.st_mode)) {
    if (DIR* directory = opendir(path.c_str())) {
      while (const dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        RemoveTreeNoFollow(path + "/" + name, error, removed);
      }
      closedir(directory);
    } else if (!error) {
      error = std::error_code(errno, std::generic_category());
    }
    if (rmdir(path.c_str()) != 0 && !error) error = std::error_code(errno, std::generic_category());
    if (removed) (*removed)();
    return;
  }
  if (unlink(path.c_str()) != 0 && !error) error = std::error_code(errno, std::generic_category());
  if (removed) (*removed)();
}
void RemoveTreeNoFollow(const std::filesystem::path& path, std::error_code& error,
                        const std::function<void()>* removed = nullptr) {
  error.clear();
  RemoveTreeNoFollow(path.string(), error, removed);
}

// Bytes of the regular files under a tree; links are not followed.
std::uint64_t SizeTreeNoFollow(const std::string& path, int depth = 0) {
  struct stat info{};
  if (depth > 24 || lstat(path.c_str(), &info) != 0) return 0;
  if (S_ISREG(info.st_mode)) return static_cast<std::uint64_t>(info.st_size);
  if (!S_ISDIR(info.st_mode)) return 0;
  std::uint64_t total = 0;
  if (DIR* directory = opendir(path.c_str())) {
    while (const dirent* entry = readdir(directory)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") total += SizeTreeNoFollow(path + "/" + name, depth + 1);
    }
    closedir(directory);
  }
  return total;
}

// Entries under a tree, the root included, for deletion progress; links are not followed.
std::uint64_t CountTreeNoFollow(const std::string& path) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) return 0;
  std::uint64_t count = 1;
  if (S_ISDIR(info.st_mode))
    if (DIR* directory = opendir(path.c_str())) {
      while (const dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") count += CountTreeNoFollow(path + "/" + name);
      }
      closedir(directory);
    }
  return count;
}

// Bytes of the regular files under a tree; links are not followed.
std::uint64_t TreeBytes(const std::filesystem::path& root) {
  std::uint64_t total = 0;
  std::error_code error;
  std::size_t entries = 0;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
       !error && it != end && ++entries < 500000; it.increment(error)) {
    std::error_code ignored;
    if (it->is_symlink(ignored)) { it.disable_recursion_pending(); continue; }
    if (it->is_regular_file(ignored)) total += it->file_size(ignored);
  }
  return total;
}

// The game's own name: a PS5 title name may carry the store, "Half-Life 2 (Steam)".
std::string WithoutStoreSuffix(std::string name) {
  static const std::string suffix = " (Steam)";
  if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
    name.resize(name.size() - suffix.size());
  return name;
}

std::optional<KeptBottleView> ReadKeptMarker(const std::filesystem::path& kept, const std::string& install_id) {
  const int fd = open((kept / kKeptMarker).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string bytes(4096, '\0');
  const auto count = read(fd, bytes.data(), bytes.size());
  close(fd);
  if (count <= 0) return std::nullopt;
  bytes.resize(static_cast<std::size_t>(count));
  const auto json = z3cloudplay::steam::json::Parse(bytes, {4096, 4, 64, 4096});
  if (!json) return std::nullopt;
  const auto* app = json->Find("app_id");
  const auto* name = json->Find("name");
  const auto* kept_at = json->Find("kept_at");
  const auto* size = json->Find("bytes");
  const auto* install = json->Find("install_id");
  if (!app || !app->is_number() || !install || !install->is_string() || install->text() != install_id) return std::nullopt;
  KeptBottleView view;
  view.install_id = install_id;
  view.app_id = static_cast<std::uint32_t>(std::strtoul(app->text().c_str(), nullptr, 10));
  if (name && name->is_string()) view.name = WithoutStoreSuffix(name->text());  // markers written before the name was stripped
  if (kept_at && kept_at->is_number()) view.kept_at = std::strtoll(kept_at->text().c_str(), nullptr, 10);
  if (size && size->is_number()) view.bytes = std::strtoull(size->text().c_str(), nullptr, 10);
  if (view.app_id == 0 || "steam-" + std::to_string(view.app_id) != install_id) return std::nullopt;
  return view;
}

// Copies a saves tree into a private bottle: directories 0700, files keep their
// owner bits, links are skipped (Wine links point outside the bottle).
bool CopySavesTree(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error) {
  struct stat info{};
  if (lstat(to.c_str(), &info) != 0) {
    if (mkdir(to.c_str(), 0700) != 0) { *error = "cannot create " + to.filename().string(); return false; }
  } else if (!S_ISDIR(info.st_mode)) {
    *error = to.filename().string() + " is not a folder"; return false;
  }
  std::error_code iteration;
  for (std::filesystem::directory_iterator it(from, iteration), end; !iteration && it != end; it.increment(iteration)) {
    const auto source = it->path();
    const auto target = to / source.filename();
    if (lstat(source.c_str(), &info) != 0) continue;
    if (S_ISLNK(info.st_mode)) continue;
    if (S_ISDIR(info.st_mode)) {
      if (!CopySavesTree(source, target, error)) return false;
      continue;
    }
    if (!S_ISREG(info.st_mode)) continue;
    const auto temporary = target.parent_path() / ("." + target.filename().string() + ".restore");
    unlink(temporary.c_str());
    const int input = open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    const int output = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                            (info.st_mode & 0700) | 0600);
    bool ok = input >= 0 && output >= 0;
    std::vector<char> buffer(64 * 1024);  // heap: console worker stacks are small
    while (ok) {
      const auto count = read(input, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = count == 0; break; }
      for (ssize_t offset = 0; ok && offset < count;) {
        const auto written = write(output, buffer.data() + offset, static_cast<std::size_t>(count - offset));
        if (written < 0 && errno == EINTR) continue;
        ok = written > 0; offset += written > 0 ? written : 0;
      }
    }
    ok = ok && fsync(output) == 0;
    if (input >= 0) close(input);
    if (output >= 0) ok = close(output) == 0 && ok;
    if (!ok || rename(temporary.c_str(), target.c_str()) != 0) {
      unlink(temporary.c_str());
      *error = "cannot copy " + source.filename().string();
      return false;
    }
  }
  if (iteration) { *error = "cannot list " + from.filename().string(); return false; }
  return true;
}
}  // namespace

std::string RuntimeHost::MaintenanceRefusal(Composition& c, const rt::RuntimeInstall& record) {
  if (record.state != rt::PreparationState::kReady) return "Prepare the game first.";
  if (record.journal) return "Finish the PS5 title's recovery first.";
  if (!c.launch_journal) return "Play is unavailable.";
  const auto pending = c.launch_journal->LoadPending();
  if (!pending.ok) return "The launch record cannot be read; restart Z3CloudPlay.";
  if (pending.session) return "Close the game first.";
  const auto activity = c.coordinator->Activity(record.title_id);
  if (activity == rt::NativeActivity::kUnknown) return "The PS5 can't tell whether the game is running. Try again.";
  if (activity != rt::NativeActivity::kStopped) return "Close the game first.";
  return {};
}

std::filesystem::path RuntimeHost::SaveBackupRoot(const std::string& install_id) const {
  return HostPath(config_.package_paths.console_data_root + "/z3cloudplay/games") / (install_id + kSaveBackupSuffix);
}

std::string RuntimeHost::RunMaintenance(Composition& c, std::uint32_t app, RuntimeMaintenance action, bool* ok) {
  *ok = false;
  std::optional<rt::RuntimeInstall> record;
  for (const auto& candidate : c.repository.Snapshot())
    if (candidate.app_id == app) record = candidate;
  if (!record) return "This game has no PS5 title yet.";
  if (const auto refused = MaintenanceRefusal(c, *record); !refused.empty()) {
    // Settings saved while the game runs reach it on the next Play from Z3CloudPlay.
    if (action == RuntimeMaintenance::kApplySettings) return "Changes apply the next time Z3CloudPlay starts the game.";
    return refused;
  }
  switch (action) {
    case RuntimeMaintenance::kApplySettings: {
      bool published = false;
      auto refused = RepublishSettings(c, *record, &published);
      if (!refused.empty()) return refused;
      *ok = true;
      return published ? "Settings applied." : "Settings are up to date.";
    }
    case RuntimeMaintenance::kClearShaderCache: {
      rt::LaunchEntry launch{record->launch_entry_id, {}, record->launch_path, record->launch_working_directory, {}};
      std::string reason;
      if (!rt::ClearShaderCache(launch, HostPath(record->install_path).string(), &reason))
        return "The shader cache could not be cleared: " + reason + ".";
      *ok = true;
      return "Shader cache cleared. Shaders compile again on the next start.";
    }
    case RuntimeMaintenance::kRepair: {
      const auto repaired = c.installer->RepairBottle(*record);
      const auto admission = c.installer->AdmissionDiagnostic(*record);
      c.admission[record->install_id] = admission;
      Log("maintenance repair app=" + std::to_string(app) + " state=" + std::to_string(static_cast<int>(repaired.state)) +
          " code=" + repaired.code + " admission=" + admission);
      PublishInstalls(c, record->install_id);
      if (repaired.state != rt::PackageActionState::kComplete) return "The bottle could not be repaired: " + repaired.message;
      if (admission != "ok") return "The PS5 title still fails its start check: " + admission;
      if (auto refused = RepublishSettings(c, *record); !refused.empty()) return refused;
      *ok = true;
      if (!c.installer->TitleCurrent(*record) || !c.installer->TitleMetadataCurrent(*record))
        return "Repaired. The PS5 title also has an update: press Update on the game page.";
      return "Repaired. The game's start files and settings are in place.";
    }
    case RuntimeMaintenance::kBackupSaves: {
      const auto users = HostPath(record->data_root) / "library/prefixes/game/drive_c/users";
      struct stat info{};
      if (lstat(users.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return "The game has no Windows user folders to back up.";
      const auto root = SaveBackupRoot(record->install_id);
      if (lstat(root.c_str(), &info) != 0 && mkdir(root.c_str(), 0700) != 0) return "The backup folder could not be made.";
      const auto stamp = std::to_string(config_.unix_time());
      const auto staging = root / ("." + stamp + ".partial");
      const auto target = root / stamp;
      std::error_code error;
      RemoveTreeNoFollow(staging, error);
      if (mkdir(staging.c_str(), 0700) != 0) return "The backup folder could not be made.";
      std::string reason;
      if (!CopySavesTree(users, staging / "users", &reason) || rename(staging.c_str(), target.c_str()) != 0) {
        RemoveTreeNoFollow(staging, error);
        Log("maintenance backup failed app=" + std::to_string(app) + " " + reason);
        return "The saves could not be backed up" + (reason.empty() ? std::string(".") : ": " + reason + ".");
      }
      // Keep the newest few; older ones are removed (and so is any partial copy).
      std::vector<std::pair<long long, std::filesystem::path>> backups;
      for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') { std::error_code ignored; RemoveTreeNoFollow(it->path(), ignored); continue; }
        backups.emplace_back(std::strtoll(name.c_str(), nullptr, 10), it->path());
      }
      std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (std::size_t index = kSaveBackupsKept; index < backups.size(); ++index) {
        std::error_code ignored;
        RemoveTreeNoFollow(backups[index].second, ignored);
      }
      Log("maintenance backup app=" + std::to_string(app) + " stamp=" + stamp);
      PublishInstalls(c, record->install_id);
      *ok = true;
      return "Saves backed up. Z3CloudPlay keeps the newest " + std::to_string(kSaveBackupsKept) + " backups.";
    }
  }
  return {};
}

std::filesystem::path RuntimeHost::HostPath(const std::string& console_path) const {
  return config_.host_namespace_prefix.empty()
      ? std::filesystem::path(console_path)
      : config_.host_namespace_prefix / std::filesystem::path(console_path).relative_path();
}

std::string RuntimeHost::SyncTitleAllowList(Composition& c) {
  if (!c.kit || std::none_of(c.kit->view.variants.begin(), c.kit->view.variants.end(),
                             [](const rt::KitTitleVariant& variant) { return variant.any_title; }))
    return {};
  if (!config_.write_title_allow_list) return "This console can't let new PS5 titles start.";
  std::set<std::string> titles;
  for (const auto& record : c.repository.Snapshot())
    if (rt::KitVariantFor(c.kit->view, record.title_id)) titles.insert(record.title_id);
  std::string contents;
  for (const auto& title : titles) contents += title + "\n";
  std::string error;
  if (!config_.write_title_allow_list(contents, &error)) {
    Log("title allow-list write failed titles=" + std::to_string(titles.size()) + " error=" + error);
    return "The list of PS5 titles allowed to start could not be written: " + error;
  }
  Log("title allow-list titles=" + std::to_string(titles.size()));
  return {};
}

bool RuntimeHost::TitleListed(Composition& c, const std::string& title_id) {
  if (c.coordinator->Inventory(title_id) != rt::TitleAvailability::kFree) return true;
  struct stat info{};
  return lstat(HostPath("/user/app/" + title_id).c_str(), &info) == 0;
}

void RuntimeHost::PublishKeptBottles(const Composition& c) {
  std::set<std::string> recorded;
  for (const auto& record : c.repository.Snapshot()) recorded.insert(record.install_id);
  std::vector<KeptBottleView> kept;
  const auto games = HostPath(config_.package_paths.console_data_root + "/z3cloudplay/games");
  std::error_code error;
  for (std::filesystem::directory_iterator it(games, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    const std::size_t suffix = std::strlen(kKeptSuffix);
    if (name.size() <= suffix || name.compare(name.size() - suffix, suffix, kKeptSuffix) != 0) continue;
    const auto install_id = name.substr(0, name.size() - suffix);
    if (!SteamInstallId(install_id) || recorded.count(install_id)) continue;
    std::error_code ignored;
    if (it->is_symlink(ignored) || !it->is_directory(ignored)) continue;
    if (auto view = ReadKeptMarker(it->path(), install_id)) {
      view->data_root = config_.package_paths.console_data_root + "/z3cloudplay/games/" + name;
      kept.push_back(std::move(*view));
    }
  }
  std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.kept_at > b.kept_at; });
  Publish([&](RuntimeSnapshot& s) { s.kept_bottles = std::move(kept); });
}

// Removal of a game's PS5 title, journaled in its install record so an interrupted
// removal continues where it stopped: retire the title folder out of ShadowMount's
// scan root, unregister the title through the one-shot remover, keep or delete the
// bottle, then drop the record once the console no longer lists the title.
std::string RuntimeHost::RunRemove(Composition& c, std::uint32_t app, bool delete_saves) {
  std::optional<rt::RuntimeInstall> found;
  for (const auto& record : c.repository.Snapshot())
    if (record.app_id == app) found = record;
  if (!found) return {};  // no PS5 title: nothing to remove
  auto record = *found;
  if (!config_.unregister_title) return "Removing PS5 titles is not available in this build.";
  if (c.launch_journal) {
    const auto pending = c.launch_journal->LoadPending();
    if (!pending.ok) return "The launch record cannot be read; restart Z3CloudPlay before uninstalling.";
    if (pending.session && pending.session->install_id == record.install_id)
      return "Close the game before uninstalling it.";
  }
  const auto activity = c.coordinator->Activity(record.title_id);
  if (activity == rt::NativeActivity::kActive) return "Close the game before uninstalling it.";
  if (activity != rt::NativeActivity::kStopped)
    return "Z3CloudPlay cannot tell whether the game is running. Close it and try again.";
  const bool resuming = record.state == rt::PreparationState::kRemoving && record.journal &&
                        record.journal->operation.rfind("remove", 0) == 0;
  // The game's name for the kept bottle, read before its title folder goes.
  std::string game_name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto named = title_names_.find(app); named != title_names_.end()) game_name = named->second;
  }
  if (game_name.empty()) game_name = InstalledTitleName(record).value_or(std::string{});
  game_name = WithoutStoreSuffix(std::move(game_name));
  auto save = [&](const char* where) -> std::string {
    const auto updated = c.repository.Update(record);
    PublishInstalls(c);
    if (!updated.ok()) {
      Log(std::string("remove record update failed at ") + where + ": " + updated.message());
      return "The removal could not be recorded: " + updated.message();
    }
    return {};
  };
  if (!resuming) {
    const auto operation = config_.uuid();
    if (!operation) return "No secure random source for the removal.";
    record.state = rt::PreparationState::kRemoving;
    record.journal = rt::PreparationJournal{*operation, delete_saves ? "remove-saves" : "remove", "retire"};
    record.last_failure.reset();
    if (auto failed = save("start"); !failed.empty()) return failed;
  }
  delete_saves = record.journal->operation == "remove-saves";
  const auto fail = [&](const std::string& code, const std::string& message) {
    record.last_failure = rt::RuntimeFailure{code, message};
    save("failure");
    Log("remove failed install=" + record.install_id + " step=" + record.journal->step + " code=" + code);
    return message;
  };
  const auto advance = [&](const char* step) {
    record.journal->step = step;
    record.last_failure.reset();
    return save(step);
  };
  Log("remove install=" + record.install_id + " title=" + record.title_id + " step=" + record.journal->step +
      " delete_saves=" + std::to_string(delete_saves));
  const auto source = config_.homebrew_root / record.title_id;
  if (record.journal->step == "retire") {
    // ShadowMount registers any folder under its scan root, so the folder leaves
    // it in one move before the title is unregistered.
    const auto manual = c.registry.RemoveManualSource(record.title_id);
    Log("remove manual source title=" + record.title_id + " ok=" + std::to_string(manual.ok) + " stage=" + manual.stage);
    struct stat info{};
    if (lstat(source.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid())
        return fail("source-foreign", "The PS5 title's folder is not Z3CloudPlay's; it was left as it is.");
      std::string reason;
      const auto trash = config_.state_root / "trash";
      if (!rt::EnsurePrivateDirectories(config_.boundary, config_.state_root, trash, &reason))
        return fail("trash-unavailable", "Z3CloudPlay could not prepare its private trash: " + reason);
      const auto retired = trash / (record.title_id + "-" + record.journal->operation_id);
      if (rename(source.c_str(), retired.c_str()) != 0)
        return fail("retire-failed", "The PS5 title's folder could not be moved away (error " +
                                     std::to_string(errno) + ").");
    } else if (errno != ENOENT) {
      return fail("source-unavailable", "The PS5 title's folder cannot be checked.");
    }
    if (auto failed = advance("unregister"); !failed.empty()) return failed;
  }
  if (record.journal->step == "unregister") {
    // Inventory first: a title the console no longer lists is never sent again.
    bool listed = TitleListed(c, record.title_id);
    TitleRemoverResult result;
    if (listed) {
      result = config_.unregister_title(record.title_id, record.journal->operation_id);
      Log("remove unregister title=" + record.title_id + " sent=" + std::to_string(result.sent) + " code=" +
          (result.code ? std::to_string(*result.code) : std::string("none")) + " detail=" + result.detail);
      for (int poll = 0; listed && poll < config_.removal_confirm_polls && !Stopping(); ++poll) {
        listed = TitleListed(c, record.title_id);
        if (!listed) break;
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, config_.removal_poll_interval, [this] { return stopping_; });
      }
      if (listed) listed = TitleListed(c, record.title_id);
    }
    if (listed) {
      if (!result.sent)
        return fail("remover-unavailable", "Z3CloudPlay could not start its title remover. Retry to try again.");
      if (result.code && *result.code != 0) {
        char code[16]; std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(*result.code));
        return fail("unregister-refused", std::string("The console refused to remove the PS5 title (") + code + ").");
      }
      // No answer is not a failure of the console: the next attempt checks again.
      return fail("unregister-unconfirmed", "The console has not confirmed that the PS5 title is gone. Retry checks again.");
    }
    if (auto failed = advance("bottle"); !failed.empty()) return failed;
  }
  if (record.journal->step == "bottle") {
    const auto bottle = HostPath(record.data_root);
    const auto kept = bottle.parent_path() / (record.install_id + kKeptSuffix);
    std::error_code error;
    struct stat info{};
    const bool present = lstat(bottle.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    if (delete_saves) {
      // Deleting a bottle takes a while: its progress is the entries removed of those counted.
      const std::uint64_t total = CountTreeNoFollow(kept.string()) + (present ? CountTreeNoFollow(bottle.string()) : 0);
      std::uint64_t done = 0;
      const std::function<void()> removed = [&] { ReportWork(++done, total); };
      RemoveTreeNoFollow(kept, error, &removed);  // links are removed, never followed
      if (!error && present) RemoveTreeNoFollow(bottle, error, &removed);
      if (!error) RemoveTreeNoFollow(SaveBackupRoot(record.install_id), error);
      ClearWork();
      if (error) return fail("bottle-delete", "The game's saves and settings could not be deleted: " + error.message());
    } else if (present) {
      const auto marker = "{\"schema\":1,\"install_id\":\"" + record.install_id + "\",\"app_id\":" +
                          std::to_string(record.app_id) + ",\"name\":\"" + JsonEscaped(game_name) +
                          "\",\"kept_at\":" + std::to_string(config_.unix_time ? config_.unix_time() : std::int64_t(time(nullptr))) +
                          ",\"bytes\":" + std::to_string(TreeBytes(bottle)) + "}\n";
      const int fd = open((bottle / kKeptMarker).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
      const bool written = fd >= 0 && write(fd, marker.data(), marker.size()) == static_cast<ssize_t>(marker.size()) &&
                           fsync(fd) == 0;
      if (fd >= 0) close(fd);
      if (!written) return fail("bottle-keep", "The game's saves could not be marked as kept.");
      if (lstat(kept.c_str(), &info) == 0) {
        // An older kept copy was never brought back; the newer bottle replaces it.
        Log("remove replaces older kept bottle install=" + record.install_id);
        RemoveTreeNoFollow(kept, error);
        if (error) return fail("bottle-keep", "An older copy of the game's saves could not be replaced.");
      }
      if (rename(bottle.c_str(), kept.c_str()) != 0)
        return fail("bottle-keep", "The game's saves could not be kept (error " + std::to_string(errno) + ").");
    }
    if (auto failed = advance("erase"); !failed.empty()) return failed;
  }
  // Erase: both the console inventory and the title folder must be gone.
  rt::NativeTitleAvailability confirmed;
  confirmed.inventory = TitleListed(c, record.title_id) ? rt::TitleAvailability::kOccupied : rt::TitleAvailability::kFree;
  struct stat info{};
  confirmed.source_path = lstat(source.c_str(), &info) != 0 && errno == ENOENT ? rt::TitleAvailability::kFree
                                                                               : rt::TitleAvailability::kOccupied;
  const auto erased = c.repository.EraseAfterUnregister(record.install_id, confirmed);
  if (!erased.ok()) return fail("erase", "The removal could not be completed: " + erased.message());
  // The title is gone; a stale entry would only let a later title with its ID start early.
  if (const auto listed = SyncTitleAllowList(c); !listed.empty()) Log("remove allow-list: " + listed);
  // A generated recipe is made again from Steam's launch options on the next prepare.
  if (rt::IsGeneratedRecipeId(record.recipe_id)) {
    c.storage.Remove(GeneratedRecipeFile(record.recipe_id));
    c.recipes.erase(std::remove_if(c.recipes.begin(), c.recipes.end(),
                                   [&](const rt::GameRecipe& recipe) { return recipe.id == record.recipe_id; }),
                    c.recipes.end());
    PublishRecipes(c);
  }
  c.admission.erase(record.install_id);
  {
    // The retired title folder: its progress is the entries removed of those counted.
    const auto trash = config_.state_root / "trash" / (record.title_id + "-" + record.journal->operation_id);
    const std::uint64_t total = CountTreeNoFollow(trash.string());
    std::uint64_t done = 0;
    const std::function<void()> removed = [&] { ReportWork(++done, total); };
    std::error_code ignored;
    RemoveTreeNoFollow(trash, ignored, &removed);
    ClearWork();
  }
  PublishInstalls(c);
  Log("remove complete install=" + record.install_id + " title=" + record.title_id);
  c.retired_titles.insert(record.title_id);
  return {};
}

std::string RuntimeHost::RestoreKeptSaves(Composition& c, std::uint32_t app) {
  std::optional<rt::RuntimeInstall> record;
  for (const auto& candidate : c.repository.Snapshot())
    if (candidate.app_id == app && (candidate.state == rt::PreparationState::kReady || AwaitingRegistration(candidate)))
      record = candidate;
  if (!record) return {};
  const auto bottle = HostPath(record->data_root);
  const auto kept = bottle.parent_path() / (record->install_id + kKeptSuffix);
  struct stat info{};
  if (lstat(kept.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return {};
  const auto from = kept / "library/prefixes/game/drive_c/users";
  const auto to = bottle / "library/prefixes/game/drive_c/users";
  std::string reason;
  if (lstat(from.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
    if (lstat(to.parent_path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
      return "The game's new bottle has no drive C for its kept saves.";
    if (!CopySavesTree(from, to, &reason)) {
      Log("restore kept saves failed app=" + std::to_string(app) + " " + reason);
      return "The saves kept from the previous install could not be brought back: " + reason;
    }
  }
  std::error_code error;
  RemoveTreeNoFollow(kept, error);
  Log("restore kept saves app=" + std::to_string(app) + " kept_removed=" + std::to_string(!error));
  PublishKeptBottles(c);
  return {};
}

std::string RuntimeHost::DeleteKept(Composition& c, std::uint32_t app) {
  const auto install_id = "steam-" + std::to_string(app);
  const auto kept = HostPath(config_.package_paths.console_data_root + "/z3cloudplay/games") / (install_id + kKeptSuffix);
  struct stat info{};
  if (lstat(kept.c_str(), &info) != 0) return {};
  if (!S_ISDIR(info.st_mode) || !ReadKeptMarker(kept, install_id)) return "This folder is not kept game data.";
  std::error_code error;
  RemoveTreeNoFollow(kept, error);  // links are removed, never followed
  if (!error) RemoveTreeNoFollow(SaveBackupRoot(install_id), error);
  return error ? "The kept saves could not be deleted: " + error.message() : std::string{};
}

void RuntimeHost::Run() {
  auto unavailable = [this](std::string reason) {
    Publish([&](RuntimeSnapshot& s) { s.status = RuntimeHostStatus::kUnavailable; s.reason = std::move(reason); });
  };
  std::string reason;
  const auto request_directory = config_.state_root / "requests";
  const bool private_ready =
      rt::EnsurePrivateDirectories(config_.boundary, config_.state_root, config_.state_root, &reason) &&
      rt::EnsurePrivateDirectories(config_.boundary, config_.state_root, request_directory, &reason);
  if (!private_ready) {
    Log("runtime unavailable: " + reason);
    unavailable("Private runtime state is unavailable: " + reason);
  }
  std::unique_ptr<Composition> composition;
  if (private_ready) {
    composition = std::make_unique<Composition>(config_);
    auto& c = *composition;
    c.request_directory = request_directory;
    if (!rt::LoadSettingsData(config_.runtime_dir, &c.presets, &c.recipes, &c.settings_data_reason) &&
        c.settings_data_reason.empty())
      c.settings_data_reason = "Game settings data is unavailable.";
    std::string kit_reason;
    c.kit = config_.admit_kit ? config_.admit_kit(&kit_reason) : std::nullopt;
    if (c.kit) {
      auto operations = config_.package_operations;
      operations.canceled = [this] { return Stopping(); };
      operations.progress = [this](std::uint64_t done, std::uint64_t total) { ReportWork(done, total); };
      operations.log = [this](const std::string& line) { Log(line); };
      c.installer = std::make_unique<rt::FilesystemPackageInstaller>(c.kit->view, config_.package_paths, operations);
    } else {
      if (kit_reason.empty()) kit_reason = "No verified compatibility kit is installed.";
      c.installer = std::make_unique<UnavailablePackageInstaller>(kit_reason);
    }
    c.coordinator = std::make_unique<ps5::Ps5NativeCoordinator>(
        c.records, *c.installer, c.registry, config_.homebrew_root, config_.native);
    c.owner = std::make_unique<rt::SettingsPublicationOwner>(c.records, *c.installer, *c.coordinator);
    c.service = std::make_unique<rt::RuntimeInstallService>(
        c.records, *c.installer, *c.coordinator,
        [&c](const rt::RuntimeInstall& record) {
          for (const auto& recipe : c.recipes)
            if (recipe.app_id == record.app_id && recipe.id == record.recipe_id &&
                std::to_string(recipe.version) == record.recipe_version)
              return rt::CompatibilityResult{true, {}};
          return rt::CompatibilityResult{false, "Z3CloudPlay no longer knows how to start this game; prepare it again."};
        },
        [this] { return Stopping(); },
        [this, &c](const rt::RuntimeInstall& record) { return DrivePublication(c, record); });
    Log("startup loading records");
    const auto loaded = c.repository.Load();
    if (!loaded.ok()) {
      Log("runtime records unavailable: " + loaded.message());
      unavailable("Runtime install records are unavailable: " + loaded.message());
      composition.reset();
    } else {
      // Startup only reads durable state. Records with a journal wait for an
      // explicit recovery action; nothing resumes, registers or rebuilds here.
      Log("startup records loaded installs=" + std::to_string(c.repository.Snapshot().size()));
      LoadGeneratedRecipes(c);
      Log("startup generated recipes loaded");
      PublishInstalls(c);
      Log("startup installs published");
      PublishKeptBottles(c);
      Log("startup kept bottles published");
      // Retired title folders left by an earlier removal are outside the scan
      // root already; deleting them is only housekeeping.
      {
        std::error_code error;
        for (std::filesystem::directory_iterator it(config_.state_root / "trash", error), end; !error && it != end;
             it.increment(error)) {
          std::error_code ignored;
          RemoveTreeNoFollow(it->path(), ignored);  // never follows links
        }
      }
      Log("startup trash emptied");
      if (const auto listed = SyncTitleAllowList(c); !listed.empty()) Log("startup allow-list: " + listed);
      if (c.kit)
        for (const auto& record : c.repository.Snapshot())
          if (record.state == rt::PreparationState::kReady) {
            // The quick form: what a game run changes and Repair fixes. Play and Update
            // run the title's whole start check.
            Log("admission check title=" + record.title_id + " quick");
            c.admission[record.install_id] = c.installer->QuickAdmission(record);
            Log("admission check title=" + record.title_id + " result=" + c.admission[record.install_id]);
          }
      PublishInstalls(c);
      if (config_.title_log)
        for (const auto& record : c.repository.Snapshot()) {
          const auto root = config_.host_namespace_prefix.empty()
              ? std::filesystem::path(record.data_root)
              : config_.host_namespace_prefix / std::filesystem::path(record.data_root).relative_path();
          const auto install = config_.host_namespace_prefix.empty()
              ? std::filesystem::path(record.install_path)
              : config_.host_namespace_prefix / std::filesystem::path(record.install_path).relative_path();
          const auto program = record.install_path.empty() || record.launch_path.empty()
              ? std::filesystem::path() : install / record.launch_path;
          if (const auto text = NewestSessionLog(root, program)) config_.title_log(record.title_id, *text);
        }
      // Play: the launch journal sits beside the install records. A session
      // pending from before a restart is polled, never handed off again.
      c.launch_journal = std::make_unique<rt::StorageLaunchSessionJournal>(c.storage);
      c.sessions = std::make_unique<rt::FilesystemReconcileSessionSource>(
          rt::HostDataRootForTests{config_.session_data_root});
      rt::LaunchDependencies launch;
      launch.preflight = [this, &c](const rt::RuntimeInstall& record) { return Preflight(c, record); };
      launch.evidence = [this, &c](const rt::RuntimeInstall& record, const std::string& session) {
        return Evidence(c, record, session);
      };
      launch.activity = [&c](const rt::RuntimeInstall& record) { return c.coordinator->Activity(record.title_id); };
      launch.backend = [this](const rt::RuntimeInstall& record, const std::string& session) {
        return config_.launch_title(record.title_id, {"session=" + session});
      };
      // Z3CloudPlay's own state is already durable; nothing waits to be written.
      launch.flush = [] { return rt::LaunchActionResult::Success(); };
      launch.uuid = [this] { return config_.uuid().value_or(std::string{}); };
      launch.now_ms = config_.now_ms;
      launch.is_application_thread = [this] { return pthread_equal(pthread_self(), application_thread_) != 0; };
      launch.evidence_wait_ms = config_.launch_evidence_wait_ms;
      {
        std::lock_guard<std::mutex> lock(launch_mutex_);
        c.launch = std::make_unique<rt::LaunchService>(c.records, *c.launch_journal, std::move(launch));
        launch_ = c.launch.get();
        launch_composition_ = &c;
      }
      if (PollLaunch(c)) {
        std::lock_guard<std::mutex> lock(mutex_);
        launch_poll_ = true;
      }
      // Settings saved while Z3CloudPlay was closed, or before a title was stopped, are published
      // now, so a start from the PS5 Home screen uses them too. Busy titles wait for Play.
      if (c.kit)
        for (const auto& record : c.repository.Snapshot()) {
          // Only newer settings: the Steam account is not known yet, so Steam files wait for Play.
          const auto saved = config_.settings_record ? config_.settings_record(record.app_id) : std::nullopt;
          if (!saved || saved->revision <= record.settings_revision || !MaintenanceRefusal(c, record).empty()) continue;
          bool published = false;
          const auto refused = RepublishSettings(c, record, &published);
          if (published || !refused.empty())
            Log("startup settings app=" + std::to_string(record.app_id) + " " + (refused.empty() ? "applied" : refused));
        }
      const auto leftovers = LeftoverRequests(c, false);
      Publish([&](RuntimeSnapshot& s) { s.leftover_requests = leftovers; });
      const bool kit_available = c.kit.has_value();
      Log("runtime ready installs=" + std::to_string(c.repository.Snapshot().size()) + " kit=" +
          (kit_available ? c.kit->view.kit_id : "none (" + kit_reason + ")") +
          (c.settings_data_reason.empty() ? std::string{} : " settings_data=" + c.settings_data_reason));
      Publish([&](RuntimeSnapshot& s) {
        s.status = RuntimeHostStatus::kReady; s.reason.clear();
        s.kit_available = kit_available; s.kit_reason = kit_available ? std::string{} : kit_reason;
        s.kit = c.kit;
        // The UI reads the kit's identity and capabilities; its file and artifact lists (thousands of
        // entries) stay on the worker, or every Snapshot() on the application thread would copy them.
        if (s.kit) {
          s.kit->view.files.clear();
          s.kit->view.files.shrink_to_fit();
          s.kit->view.artifacts.clear();
          s.kit->view.artifacts.shrink_to_fit();
        }
      });
    }
  }
  for (;;) {
    std::vector<RuntimePrepareRequest> requests;
    std::vector<std::uint32_t> plays;
    std::vector<std::pair<std::uint32_t, bool>> removals;
    std::vector<std::uint32_t> kept_deletes;
    std::vector<std::pair<std::uint32_t, RuntimeMaintenance>> maintenance;
    std::vector<std::pair<std::uint32_t, std::string>> scans;
    bool poll = false;
    bool cleanup = false;
    bool measure = false;
    std::vector<std::uint32_t> checks;
    bool names_changed = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Preparation changes install records that Handoff reads on the
      // application thread, so it waits while a handoff is pending.
      const auto ready = [this] {
        return stopping_ || (!prepare_requests_.empty() && !snapshot_.handoff_ready) || !play_requests_.empty() ||
               cleanup_requested_ || measure_requested_ || !activity_requests_.empty() || title_names_changed_ ||
               (!remove_requests_.empty() && !snapshot_.handoff_ready) || !kept_delete_requests_.empty() ||
               !scan_requests_.empty() ||
               (!maintenance_requests_.empty() && !snapshot_.handoff_ready);
      };
      if (launch_poll_) wake_.wait_for(lock, kLaunchPollInterval, ready);
      else wake_.wait(lock, [&] { return ready() || launch_poll_; });
      if (stopping_) break;
      if (!snapshot_.handoff_ready) requests.swap(prepare_requests_);
      plays.swap(play_requests_);
      // Removal changes install records that Handoff reads, like preparation.
      if (!snapshot_.handoff_ready) removals.swap(remove_requests_);
      kept_deletes.swap(kept_delete_requests_);
      scans.swap(scan_requests_);
      // Maintenance changes the bottle and install record, like removal.
      if (!snapshot_.handoff_ready) maintenance.swap(maintenance_requests_);
      poll = launch_poll_;
      cleanup = cleanup_requested_;
      cleanup_requested_ = false;
      measure = measure_requested_;
      measure_requested_ = false;
      checks.swap(activity_requests_);
      names_changed = title_names_changed_;
      title_names_changed_ = false;
    }
    if (names_changed && composition) PublishInstalls(*composition);
    for (const auto& [app, install_path] : scans) {
      const auto kit = composition && composition->kit
          ? rt::KitCapabilitiesFromManifest(composition->kit->capabilities, true) : rt::KitCapabilities{};
      auto programs = rt::ListGamePrograms(HostPath(install_path).string(), kit);
      Log("program scan app=" + std::to_string(app) + " programs=" + std::to_string(programs.size()));
      Publish([&](RuntimeSnapshot& s) {
        auto& setup = s.game_setups[app];
        setup.programs = std::move(programs);
        setup.scanned = true;
      });
    }
    for (const auto app : checks) {
      // Unknown unless the console says otherwise; no runtime means unknown too.
      auto activity = rt::NativeActivity::kUnknown;
      if (composition) {
        activity = rt::NativeActivity::kStopped;
        for (const auto& record : composition->repository.Snapshot())
          if (record.app_id == app && !record.title_id.empty()) activity = composition->coordinator->Activity(record.title_id);
      }
      Publish([&](RuntimeSnapshot& s) { s.activity[app] = {activity, ++s.activity_serial}; });
    }
    if (measure && composition) {
      std::map<std::uint32_t, std::uint64_t> sizes;
      constexpr const char* kUsers = "/library/prefixes/game/drive_c/users";
      for (const auto& record : composition->repository.Snapshot())
        if (!record.data_root.empty()) sizes[record.app_id] = SizeTreeNoFollow(HostPath(record.data_root + kUsers).string());
      const auto kept = Snapshot().kept_bottles;
      for (const auto& bottle : kept)
        if (!sizes.count(bottle.app_id)) sizes[bottle.app_id] = SizeTreeNoFollow(HostPath(bottle.data_root + kUsers).string());
      Publish([&](RuntimeSnapshot& s) { s.save_bytes = std::move(sizes); });
    }
    if (cleanup && composition) {
      const auto kept = LeftoverRequests(*composition, true);
      const auto left = LeftoverRequests(*composition, false);
      Log("leftover requests cleanup kept=" + std::to_string(kept) + " remaining=" + std::to_string(left));
      Publish([&](RuntimeSnapshot& s) { s.leftover_requests = left; });
    }
    for (const auto app : plays) {
      if (composition) {
        RunPlay(*composition, app);
        poll = true;
      } else {
        Publish([&](RuntimeSnapshot& s) {
          s.play_refusals[app] = s.reason.empty() ? "The runtime services are unavailable." : s.reason;
          ++s.plays_handled;
        });
      }
    }
    for (const auto& [app, delete_saves] : removals) {
      std::string refusal;
      if (!composition) {
        const auto current = Snapshot();
        refusal = current.reason.empty() ? "The runtime services are unavailable." : current.reason;
      } else {
        Publish([&](RuntimeSnapshot& s) { s.removing_app = app; s.removal_failures.erase(app); });
        refusal = RunRemove(*composition, app, delete_saves);
        PublishKeptBottles(*composition);
      }
      Log("remove result app=" + std::to_string(app) + " " + (refusal.empty() ? "removed" : refusal));
      ClearWork();
      Publish([&](RuntimeSnapshot& s) {
        s.removing_app.reset();
        if (refusal.empty()) s.removal_failures.erase(app);
        else s.removal_failures[app] = refusal;
        ++s.removals_handled;
      });
    }
    for (const auto app : kept_deletes) {
      if (!composition) continue;
      const auto refusal = DeleteKept(*composition, app);
      Log("kept bottle delete app=" + std::to_string(app) + " " + (refusal.empty() ? "deleted" : refusal));
      PublishKeptBottles(*composition);
      if (!refusal.empty()) Publish([&](RuntimeSnapshot& s) { s.removal_failures[app] = refusal; });
    }
    for (const auto& [app, action] : maintenance) {
      bool ok = false;
      const auto message = composition ? RunMaintenance(*composition, app, action, &ok)
                                       : std::string("The runtime services are unavailable.");
      Log("maintenance app=" + std::to_string(app) + " action=" + std::to_string(static_cast<int>(action)) +
          " ok=" + std::to_string(ok) + " " + message);
      ClearWork();
      Publish([&](RuntimeSnapshot& s) {
        s.maintenance[app] = {action, false, ok, message};
        ++s.maintenance_handled;
      });
    }
    if (poll && composition) {
      const bool again = PollLaunch(*composition);
      std::lock_guard<std::mutex> lock(mutex_);
      launch_poll_ = again;
    }
    for (std::size_t index = 0; index < requests.size(); ++index) {
      const auto& request = requests[index];
      // A handoff that became ready during this pass reads install records on
      // the application thread: hold the remaining preparations until it is done.
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.handoff_ready) {
          prepare_requests_.insert(prepare_requests_.begin(), requests.begin() + static_cast<std::ptrdiff_t>(index),
                                   requests.end());
          break;
        }
      }
      std::string refusal;
      bool awaiting = false;
      if (!composition) {
        const auto current = Snapshot();
        refusal = current.reason.empty() ? "The runtime services are unavailable." : current.reason;
      } else {
        Publish([&](RuntimeSnapshot& s) {
          s.preparing_app = request.app_id;
          s.queued_prepares.erase(request.app_id);
          s.refusals.erase(request.app_id);
        });
        refusal = RunPrepare(*composition, request, &awaiting);
        // A game prepared again gets back the saves it kept when it was uninstalled,
        // before it can first be started (from Home, while it waits for registration).
        if (refusal.empty()) refusal = RestoreKeptSaves(*composition, request.app_id);
        // Settings saved while the game was being prepared (after its profile was written) reach
        // the title as soon as it is ready; the check at startup ran before that.
        if (refusal.empty() && !awaiting)
          for (const auto& record : composition->repository.Snapshot()) {
            if (record.app_id != request.app_id) continue;
            bool published = false;
            const auto refused = RepublishSettings(*composition, record, &published);
            if (published || !refused.empty())
              Log("prepared settings app=" + std::to_string(record.app_id) + " " + (refused.empty() ? "applied" : refused));
          }
      }
      Log("prepare result app=" + std::to_string(request.app_id) + " " +
          (!refusal.empty() ? refusal : awaiting ? "awaiting registration" : "ready"));
      if (composition) {
        const auto leftovers = LeftoverRequests(*composition, false);
        Publish([&](RuntimeSnapshot& s) { s.leftover_requests = leftovers; });
      }
      ClearWork();
      Publish([&](RuntimeSnapshot& s) {
        s.preparing_app.reset();
        s.queued_prepares.erase(request.app_id);
        ++s.prepares_handled;
        if (refusal.empty()) s.refusals.erase(request.app_id);
        else s.refusals[request.app_id] = refusal;
      });
    }
  }
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    launch_ = nullptr;
    launch_composition_ = nullptr;
  }
  composition.reset();
  Publish([](RuntimeSnapshot& s) { s.status = RuntimeHostStatus::kStopped; });
}

}  // namespace z3cloudplay::homebrew
