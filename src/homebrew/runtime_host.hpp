#pragma once
// runtime_host.hpp - Worker-owned composition of the per-game runtime services.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include "platform/ps5/native_coordinator.h"
#include "platform/ps5/title_registry.h"
#include "platform/ps5/title_runtime.h"
#include "runtime/game_install.h"
#include "runtime/game_settings.h"
#include "runtime/install_service.h"
#include "runtime/launch_service.h"
#include "runtime/package_installer.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <string>
#include <vector>

namespace z3cloudplay::homebrew {

// An admitted kit: the verified view and the capabilities its manifest declares.
struct RuntimeKit {
  runtime::VerifiedKitView view;
  std::vector<std::string> capabilities;
  std::string manifest_sha256;
  std::string expected_manifest_sha256;  // the kit this build was tested with
  std::string prospero_win_revision;     // what its PS5 titles were built from
  bool source_dirty = false;
};

// A game's private bottle kept after its PS5 title was removed: its saves and
// settings, until the game is prepared again or the player deletes it.
struct KeptBottleView {
  std::string install_id;
  std::uint32_t app_id = 0;
  std::string name;          // the game's name when it was kept
  std::int64_t kept_at = 0;  // Unix time
  std::uint64_t bytes = 0;   // size when it was kept
  std::string data_root;     // the kept bottle's folder (its library/prefixes/game holds the saves)
};

// What the one-shot title remover reported: whether it was sent, its result code
// when it answered, and a diagnostic.
struct TitleRemoverResult {
  bool sent = false;
  std::optional<int> code;
  std::string detail;
};

struct RuntimeHostConfig {
  // Existing trusted parent of state_root (for example /data).
  std::filesystem::path boundary;
  // Private 0700 OWNER state: installs.json and requests/ (settings requests).
  std::filesystem::path state_root;
  std::filesystem::path homebrew_root = "/data/homebrew";
  // Host-test namespace prefix for console paths; empty on the console.
  std::filesystem::path host_namespace_prefix;
  // Empty members use the native SQLite inventory and process activity readers.
  ps5::NativeCoordinatorOperations native;
  ps5::TitleRegistry::Transport registry_transport;
  // Verifies the installed kit on the worker, or returns nothing with a reason.
  std::function<std::optional<RuntimeKit>(std::string* reason)> admit_kit;
  // Package layer paths and base operations (hashing, sync, space, bottle
  // validation). The host adds cancellation.
  runtime::PackageInstallPaths package_paths;
  runtime::PackageInstallerOperations package_operations;
  // Bundled recipes/presets/input presets (<title>/runtime).
  std::string runtime_dir;
  // gbe_fork's steam_api DLLs; empty: <runtime_dir>/steamworks.
  std::string steamworks_dir;
  // The player's saved settings for a game; nothing means Z3CloudPlay defaults.
  std::function<std::optional<runtime::GameSettingsRecord>(std::uint32_t app_id)> settings_record;
  // Canonical lower-case UUID for a publication transaction, or nothing when no
  // secure random source is available. Empty uses /dev/urandom.
  std::function<std::optional<std::string>()> uuid;
  std::function<std::int64_t()> unix_time;
  // Diagnostic lines (no credentials): startup, each preparation step and refusals.
  std::function<void(const std::string&)> log;
  // Receives each prepared title's newest wrapper session log (bounded tail) at
  // startup. The wrapper writes it inside the private bottle tree, which only
  // the app user can read.
  std::function<void(const std::string& title_id, const std::string& text)> title_log;
  // The game's PS5 title icon: a 512x512 PNG made from its store artwork, or
  // nothing (the kit's icon stays). Runs on the worker; may use the network.
  std::function<std::optional<std::string>(std::uint32_t app_id)> title_icon;
  // Native start of a prepared title with its arguments ("session=<UUID>").
  // Called only on the application thread, from Handoff. Empty disables Play.
  std::function<ps5::TitleResult(const std::string& title_id, const std::vector<std::string>& arguments)> launch_title;
  // Unregisters a game's PS5 title through the one-shot title remover (worker
  // only; bounded). The nonce ties its answer to this request. Empty disables
  // removal of PS5 titles.
  std::function<TitleRemoverResult(const std::string& title_id, const std::string& nonce)> unregister_title;
  // Replaces the root-owned allow-list (runtime::kTitleRuleAllowList) that a kit's any-title
  // helper reads with these bytes: one title ID per line (worker only). Empty: a kit with one
  // shell for every game can't prepare games.
  std::function<bool(const std::string& contents, std::string* error)> write_title_allow_list;
  // How long removal waits for the console to drop an unregistered title.
  int removal_confirm_polls = 30;
  std::chrono::milliseconds removal_poll_interval{1000};
  // Canonical /data as this process sees it, for the wrapper's session records.
  std::filesystem::path session_data_root = "/data";
  std::function<std::int64_t()> now_ms;
  // How long a started title may take to report its session before Play gives up.
  std::int64_t launch_evidence_wait_ms = 60000;
  // SQLite and the native readers run on this worker. PS5 std::thread stacks are
  // too small for them, so the worker uses an explicit stack size.
  std::size_t stack_bytes = 1024 * 1024;
};

struct RuntimeInstallView {
  std::string install_id;
  std::uint32_t app_id = 0;
  std::string title_id;
  // The game's private bottle (console path); Cloud saves under Windows
  // folders live here.
  std::string prefix_root;
  runtime::PreparationState state = runtime::PreparationState::kDownloaded;
  std::string step;     // current journal step, if any
  std::string failure;
  // A durable journal is present while no preparation runs. Recovery is
  // explicit; startup never resumes it.
  bool recovery_pending = false;
  // Staged and its registration submitted: ShadowMountPlus registers it once no
  // title runs, so the game is ready to play from the PS5 Home screen.
  bool awaiting_registration = false;
  // False when a Ready title was made from an earlier kit; Prepare updates it.
  bool title_current = true;
  // Publication admission result for Ready titles ("ok" or the refusal); a
  // refusal means the next start would be refused, and Prepare repairs it.
  std::string admission = "ok";
  // The settings revision its published profile was made from, and the launch entry the
  // title was prepared with (a different Launch option needs the bottle made again).
  std::uint64_t settings_revision = 0;
  std::string launch_entry_id;
  std::string launch_path;               // its program, relative to the install root
  std::string launch_working_directory;  // its folder; empty: the program's own
  // Save backups kept for the game, and when the newest was made (Unix time).
  std::size_t save_backups = 0;
  std::int64_t latest_save_backup = 0;
};

// Work on a prepared game's files, run while its PS5 title is stopped.
enum class RuntimeMaintenance {
  kApplySettings,     // publish the saved settings when they are newer than the title's
  kClearShaderCache,  // delete DXVK's pipeline cache for the launch program
  kRepair,            // fix the bottle's start files, then apply the settings
  kBackupSaves,       // copy the bottle's Windows user folders aside
};

// The newest maintenance request for one game.
struct RuntimeMaintenanceView {
  RuntimeMaintenance action = RuntimeMaintenance::kApplySettings;
  bool running = false;
  bool ok = false;
  std::string message;  // the outcome, for the player
};

// Play: the launch in progress, or how the last one ended.
struct RuntimeLaunchView {
  std::string install_id;
  std::uint32_t app_id = 0;
  std::string title_id;
  runtime::LaunchState state = runtime::LaunchState::kIdle;
  std::string code;
  std::string message;
  std::optional<int> exit_code;
};

enum class RuntimeHostStatus { kStarting, kUnavailable, kReady, kStopped };

// What Game settings needs to pick a game's program: the Windows programs in its folder and,
// for a game Z3CloudPlay could not prepare, a draft recipe to start the sheet from.
struct RuntimeGameSetup {
  std::vector<runtime::GameProgram> programs;
  bool scanned = false;  // programs is the folder's latest scan
  // Set when preparing was refused: the recipe the player's choices start from (it may list
  // no launch entry yet) and whether a choice (a program, "Try anyway") can get past the refusal.
  std::optional<runtime::GameRecipe> draft;
  bool refusal_soft = false;
};


struct RuntimeSnapshot {
  std::uint64_t revision = 0;
  RuntimeHostStatus status = RuntimeHostStatus::kStarting;
  std::string reason;  // why the runtime services are unavailable
  bool kit_available = false;
  std::string kit_reason;  // why preparation is unavailable
  std::optional<RuntimeKit> kit;  // the admitted kit, for display
  std::vector<RuntimeInstallView> installs;
  // Latest refused or failed request per Steam app, for presentation, and how many
  // preparation requests the worker has finished.
  std::map<std::uint32_t, std::string> refusals;
  std::uint64_t prepares_handled = 0;
  std::optional<std::uint32_t> preparing_app;  // the game being prepared now
  // Games whose preparation is asked for but not started yet: it waits behind another game's
  // preparation, or for the runtime to finish starting.
  std::set<std::uint32_t> queued_prepares;
  // How far the current copy (preparing_app) or deletion (removing_app) is, when
  // that is measurable; absent for steps whose progress is unknown.
  std::optional<float> work_progress;
  std::optional<RuntimeLaunchView> launch;
  // Settings requests no install record's journal names: left by an interrupted
  // preparation, a refused request or a finished publication. They grant nothing.
  std::size_t leftover_requests = 0;
  // Answers to CheckActivity: the title's activity per Steam app, with the
  // check's serial so a caller can tell a fresh answer from an older one.
  std::map<std::uint32_t, std::pair<runtime::NativeActivity, std::uint64_t>> activity;
  std::uint64_t activity_serial = 0;
  // Play requests the worker has finished handling (accepted or refused).
  std::uint64_t plays_handled = 0;
  // Latest refused Play per Steam app, for presentation.
  std::map<std::uint32_t, std::string> play_refusals;
  // A checked session waits for Handoff on the application thread.
  bool handoff_ready = false;
  // Recipes Z3CloudPlay generated from Steam launch options, for the settings sheet.
  std::vector<runtime::GameRecipe> generated_recipes;
  // Program choices per Steam app, from ScanPrograms and from preparation.
  std::map<std::uint32_t, RuntimeGameSetup> game_setups;
  // Removal: the game whose PS5 title is being removed now, the latest refusal or
  // failure per Steam app, and how many removal requests the worker has finished.
  std::optional<std::uint32_t> removing_app;
  std::map<std::uint32_t, std::string> removal_failures;
  std::uint64_t removals_handled = 0;
  // Game data kept when a game was uninstalled (bottles with no install record).
  std::vector<KeptBottleView> kept_bottles;
  // Maintenance per Steam app, and how many requests the worker has finished.
  std::map<std::uint32_t, RuntimeMaintenanceView> maintenance;
  std::uint64_t maintenance_handled = 0;
  // Bytes in each game's Windows user folders (installed and kept), from the latest MeasureSaves.
  std::map<std::uint32_t, std::uint64_t> save_bytes;
};

// A downloaded game to prepare. The game files stay at install_path.
struct RuntimePrepareRequest {
  std::uint32_t app_id = 0;
  std::string install_path;  // console path of the Steam install
  std::string volume_id;
  std::string title_name;    // the game's name, shown for its PS5 title
  // Steam's Windows launch options (label = Steam's description), used when Z3CloudPlay
  // ships no recipe for the game. launch_reason says why they are missing.
  std::vector<runtime::LaunchEntry> launches;
  std::string launch_reason;
};

// The worker constructs and owns every runtime collaborator, so durable work and
// native inventory never run on the main thread. The main thread reads snapshots
// and queues requests. Play is split the same way: the worker checks the game
// and prepares the launch session, and only Handoff, on the application thread
// that constructed this object, starts the title.
class RuntimeHost {
 public:
  explicit RuntimeHost(RuntimeHostConfig config);
  ~RuntimeHost();
  RuntimeHost(const RuntimeHost&) = delete;
  RuntimeHost& operator=(const RuntimeHost&) = delete;

  bool Start();
  void Shutdown();
  RuntimeSnapshot Snapshot() const;
  // The same state without a copy per call: shared and read-only, made again only after a change.
  // The UI reads it many times a frame; Snapshot() copies every field each time.
  std::shared_ptr<const RuntimeSnapshot> Shared() const;
  // Queues preparation of a downloaded game, or a retry of a failed one.
  void Prepare(RuntimePrepareRequest request);
  // Asks the console whether a game's PS5 title runs (from any origin, Home
  // included). A game with no prepared title reports stopped.
  void CheckActivity(std::uint32_t app_id);
  // CheckActivity, waiting up to `timeout` for the answer (kUnknown without one). For other
  // threads, such as the Cloud worker checking a game just before moving its saves.
  runtime::NativeActivity ActivityNow(std::uint32_t app_id, std::chrono::milliseconds timeout);
  // Queues removal of the leftover settings requests (rechecked on the worker).
  void CleanUpRequests();
  // The names each game's PS5 title should carry. A Ready title whose installed
  // name differs reports an update, and Prepare applies the new name.
  void SetTitleNames(std::map<std::uint32_t, std::string> names);
  // Queues Play of a Ready game.
  void Play(std::uint32_t app_id);
  // The signed-in Steam account, for the Steamworks emulator's settings (empty when signed out).
  void SetSteamAccount(std::string account_name, std::uint64_t steam_id);
  // Queues removal of a game's PS5 title. Its bottle (saves and settings) is kept
  // unless delete_saves; an interrupted removal continues with its first choice.
  void Remove(std::uint32_t app_id, bool delete_saves);
  // Queues deletion of a game's kept bottle.
  void DeleteKeptBottle(std::uint32_t app_id);
  // Queues a scan of a game folder for the Program picker (RuntimeSnapshot::game_setups).
  void ScanPrograms(std::uint32_t app_id, std::string install_path);
  // Measures each game's Windows user folders (where saves of games without Steam Cloud live).
  void MeasureSaves();
  // Queues maintenance of a prepared game. Each action is refused while the game's
  // title runs or its state is unknown.
  void Maintain(std::uint32_t app_id, RuntimeMaintenance action);
  // Application thread only: starts the title of a prepared launch session.
  // Returns false when no session is waiting.
  bool Handoff();

 private:
  struct Composition;
  static void* Entry(void* self);
  void Run();
  void Publish(const std::function<void(RuntimeSnapshot&)>& change);
  // With `only`, that install's title is checked again and the others keep their last
  // check (about two seconds per title on the console).
  void PublishInstalls(const Composition& composition, const std::string& only = {});
  // Empty when the title is Ready or, with *awaiting set, staged and waiting for the
  // console to register it; otherwise why not.
  std::string RunPrepare(Composition& composition, const RuntimePrepareRequest& request, bool* awaiting = nullptr);
  const runtime::GameRecipe* RecipeForPrepare(Composition& composition, const RuntimePrepareRequest& request,
                                              std::string* reason);
  void LoadGeneratedRecipes(Composition& composition);
  void PublishRecipes(const Composition& composition);
  runtime::PackageActionResult DrivePublication(Composition& composition, const runtime::RuntimeInstall& record);
  // Puts the saved settings into a Ready title when they are newer than its publication:
  // Steam files, then a journaled settings update (profile, dxvk.conf, registry). Call only
  // while the title is stopped. Empty on success or when nothing changed; *published says
  // whether an update ran. The title keeps the launch entry it was prepared with.
  std::string RepublishSettings(Composition& composition, const runtime::RuntimeInstall& record,
                                bool* published = nullptr);
  // Empty when the game's Ready title is stopped with no launch pending, otherwise why not.
  std::string MaintenanceRefusal(Composition& composition, const runtime::RuntimeInstall& record);
  // Runs one maintenance action; empty message on refusal means none; *ok tells success.
  std::string RunMaintenance(Composition& composition, std::uint32_t app_id, RuntimeMaintenance action, bool* ok);
  std::filesystem::path SaveBackupRoot(const std::string& install_id) const;
  // Puts the game's steam_api DLLs in the state its "Steam features" setting asks for. Call
  // only while the game is stopped; empty on success, otherwise why not (for the player).
  std::string ApplySteamworks(Composition& composition, const runtime::RuntimeInstall& record);
  // Makes an owned, world-writable folder from root down to working 0755 (dxvk.conf goes there).
  std::string TightenWorkingFolder(const std::filesystem::path& root, const std::string& working);
  runtime::LaunchPreflight Preflight(Composition& composition, const runtime::RuntimeInstall& record);
  runtime::LaunchEvidence Evidence(Composition& composition, const runtime::RuntimeInstall& record,
                                   const std::string& session_id);
  void RunPlay(Composition& composition, std::uint32_t app_id);
  // Removes a game's PS5 title; empty when it has none left, otherwise why not.
  std::string RunRemove(Composition& composition, std::uint32_t app_id, bool delete_saves);
  // Whether the console still lists the title (app.db or /user/app); unknown counts as listed.
  bool TitleListed(Composition& composition, const std::string& title_id);
  // Moves the saves of a kept bottle into the game's new bottle and drops the kept copy.
  std::string RestoreKeptSaves(Composition& composition, std::uint32_t app_id);
  std::string DeleteKept(Composition& composition, std::uint32_t app_id);
  void PublishKeptBottles(const Composition& composition);
  // With an any-title kit, lists every recorded game title in the helper's allow-list so their
  // titles may start; empty on success (or with no such kit), otherwise why not.
  std::string SyncTitleAllowList(Composition& composition);
  std::filesystem::path HostPath(const std::string& console_path) const;
  std::optional<std::string> InstalledTitleName(const runtime::RuntimeInstall& record) const;
  bool TitleNameCurrent(const runtime::RuntimeInstall& record) const;
  // Writes the game's own title name (and icon) for its PS5 title.
  runtime::PackageActionResult WriteTitleMetadata(Composition& composition, const runtime::RuntimeInstall& record,
                                                  const std::string& title_name);
  // Polls a pending launch session and ends it once its outcome is final.
  // Returns true while a session is still undecided.
  bool PollLaunch(Composition& composition);
  void PublishLaunch(Composition& composition, const runtime::LaunchSnapshot& launch);
  // Settings request files that no record's journal names; with remove, deletes them.
  std::size_t LeftoverRequests(Composition& composition, bool remove);
  bool Stopping() const;
  void Log(const std::string& line) const;
  // Worker thread: publishes the work fraction when its whole percent changes; ClearWork removes it.
  void ReportWork(std::uint64_t done, std::uint64_t total);
  void ClearWork();

  RuntimeHostConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable published_;  // notified on every Publish
  RuntimeSnapshot snapshot_;
  mutable std::shared_ptr<const RuntimeSnapshot> shared_;  // Shared()'s copy of snapshot_ at shared_revision_
  mutable std::uint64_t shared_revision_ = 0;
  std::vector<RuntimePrepareRequest> prepare_requests_;
  std::vector<std::uint32_t> play_requests_;
  std::vector<std::pair<std::uint32_t, bool>> remove_requests_;
  std::vector<std::uint32_t> kept_delete_requests_;
  std::vector<std::pair<std::uint32_t, std::string>> scan_requests_;
  std::vector<std::pair<std::uint32_t, RuntimeMaintenance>> maintenance_requests_;
  std::vector<std::uint32_t> activity_requests_;
  int work_percent_ = -1;  // worker thread only: the published work percent, -1 for none
  bool launch_poll_ = false;
  bool cleanup_requested_ = false;
  bool measure_requested_ = false;
  std::map<std::uint32_t, std::string> title_names_;  // guarded by mutex_
  std::string steam_account_name_;                     // guarded by mutex_
  std::uint64_t steam_id_ = 0;                         // guarded by mutex_
  bool title_names_changed_ = false;
  // Guards the worker's launch service and preflight cache for Handoff.
  std::mutex launch_mutex_;
  runtime::LaunchService* launch_ = nullptr;
  Composition* launch_composition_ = nullptr;
  pthread_t application_thread_{};
  bool stopping_ = false;
  bool started_ = false;
  void* thread_ = nullptr;  // pthread_t storage, owned by the implementation
};

// Canonical random (version 4) UUID from /dev/urandom, or nothing.
std::optional<std::string> RandomUuid();

}  // namespace z3cloudplay::homebrew
