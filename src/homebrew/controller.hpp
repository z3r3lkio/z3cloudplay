#pragma once
#include <array>
#include "homebrew/simulation.hpp"
#include "homebrew/uninstall_flow.hpp"
#include "steam/service.h"
#include "app/library_controller.h"
#include "app/persistence_controller.h"
#include "steam/app_details_service.h"
#include "steam/store_details_service.h"
#include "steam/install_service.h"
#include "steam/cloud_service.h"
#include "homebrew/runtime_host.hpp"
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>

namespace z3cloudplay::homebrew {
// Main-thread bridge. The backend/repository outlive this object. IO stays on service
// workers; the presentation receives no credential tokens.
class Controller {
 public:
  Controller(Simulation& model, steam::AuthBackend& backend, SessionRepository& repository,
             LibraryService* library_service = nullptr, app::PersistenceService* persistence_service = nullptr,
             steam::AppDetailsService* app_details = nullptr, steam::InstallService* installs = nullptr,
             steam::CloudService* cloud = nullptr, RuntimeHost* runtime = nullptr,
             bool defer_initial_restore = false);
  ~Controller();
  void Tick();
  // Microseconds each part of the last Tick took, for slow-frame diagnostics.
  std::string LastTickProfile() const;
  void Shutdown();
  // Reattempt restoration once the session vault has been unlocked.
  void RestoreAfterUnlock();
  // Steam store descriptions, genres and controller support for the games the player opens.
  void SetStoreDetails(steam::StoreDetailsService* store_details) { store_details_ = store_details; }
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;
 private:
  enum class Phase { Idle, Restore, SignIn, ClearLibrary, ClearOrganization, SignOut };
  void Begin();
  void SignOut(bool switch_account);
  void Reauthenticate();
  void Restore();
  void AdvanceClear();
  void BeginOrganizationClear();
  void AdvanceOrganizationClear();
  void Apply(const steam::SteamEvent& event);
  void Update(AuthState state, const std::string& error = {}, bool clear = false,
              const std::string& challenge = {});
  void TickAppDetails();
  void TickStoreDetails();
  steam::StoreDetailsService* store_details_ = nullptr;
  std::optional<GameId> requested_store_detail_;
  void TickInstalls();
  void TickCloud();
  void TickRuntime();
  Simulation& model_;
  steam::AppDetailsService* app_details_ = nullptr;
  steam::InstallService* installs_ = nullptr;
  std::uint64_t install_revision_ = 0;
  std::uint64_t install_generation_ = 0;
  steam::CloudService* cloud_ = nullptr;
  std::uint64_t cloud_revision_ = 0;
  std::map<GameId, std::string> game_roots_;  // installed games' folders, for Steam Cloud
  std::map<GameId, std::string> game_volumes_;  // installed games' volume ids
  // Steam's Windows launch options per game, from app details. A game Z3CloudPlay ships
  // no recipe for is prepared from them; Prepare waits while they are fetched.
  std::map<GameId, std::vector<steam::LaunchOption>> launch_options_;
  std::map<GameId, std::string> launch_options_reason_;
  std::set<GameId> prepare_after_details_;
  // announce: tell the player when this preparation leaves the game ready to play
  // (waiting for the PS5 to register it); the startup check does not.
  void SubmitPrepare(GameId id, bool announce = true);
  RuntimeHost* runtime_ = nullptr;
  std::uint64_t runtime_revision_ = 0;
  std::uint64_t runtime_install_revision_ = 0;
  std::uint64_t runtime_cloud_revision_ = 0;
  std::string saves_root_;                    // per-game save state folders, on the primary drive
  // Save lifecycle around Play: games whose session just ended sync once their
  // folder is known; games syncing now cannot start.
  std::set<GameId> sync_after_session_;
  std::set<GameId> cloud_busy_;
  // Play requested but not yet visible in the runtime's launch state.
  std::set<GameId> play_queued_;
  std::uint64_t plays_requested_ = 0;
  // Play checks Cloud saves first: games waiting for that check (true once the
  // sync was issued), games whose check passed and can start, games the player
  // may start without syncing after a failed check, and what Play shows meanwhile.
  std::map<GameId, bool> sync_before_play_;
  std::vector<GameId> plays_ready_;
  std::set<GameId> play_without_sync_;
  // Games are prepared without a button press: once their download finishes (games
  // downloading now, then games waiting to be prepared), and when Play finds one not
  // prepared yet. Play then starts it once it is Ready; the refusal seen when Prepare
  // was queued tells a new refusal from an old one.
  std::set<GameId> downloading_;
  std::set<GameId> auto_prepare_;
  struct PlayAfterPrepare {
    bool queued = false;
    bool seen_preparing = false;
    std::string refusal;
    std::uint64_t prepares_handled = 0;  // the worker's count when this prepare was queued
    std::chrono::steady_clock::time_point queued_at{};
    std::string words = "Preparing…";  // what Play says meanwhile: preparing, updating or repairing the runtime
  };
  std::map<GameId, PlayAfterPrepare> play_after_prepare_;
  // Preparations the player started (Play, Prepare, a finished download): when one
  // ends waiting for the PS5, the model asks whether to quit to Home to play it.
  struct ReadyWatch {
    bool seen_preparing = false;
    std::chrono::steady_clock::time_point submitted_at{};
  };
  std::map<GameId, ReadyWatch> ready_watch_;
  std::vector<GameId> ready_notices_;  // ready to play, announced after the next projection
  // Recreate bottle: the PS5 title is removed with its saves kept, then the game is
  // prepared again (which brings the saves back). Results stay for the maintenance sheet.
  struct Recreate {
    std::uint64_t removals_before = 0;
    bool prepare_submitted = false;
    bool seen_preparing = false;
    std::string refusal;  // the game's Prepare refusal when it was submitted
    std::chrono::steady_clock::time_point submitted_at{};
  };
  std::map<GameId, Recreate> recreate_;
  std::map<GameId, std::pair<bool, std::string>> recreate_results_;
  // The verified kit's capabilities, which gate Game settings rows.
  std::optional<std::vector<std::string>> kit_capabilities_;
  // Titles waiting for the PS5 when Z3CloudPlay started are checked once (query only).
  bool startup_awaiting_known_ = false;
  std::set<GameId> startup_awaiting_;
  // Uninstall order: Cloud saves, then the PS5 title, then the download.
  UninstallFlow uninstalls_;
  void BeginUninstall(GameId id, bool delete_saves);
  void StopUninstall(GameId id, const std::string& reason);
  void TickUninstalls();
  std::uint64_t kept_revision_ = 0;
  std::vector<LiveKeptBottle> kept_projected_;
  std::map<GameId, std::string> play_notes_;
  std::uint64_t play_notes_revision_ = 0;
  std::uint64_t runtime_notes_revision_ = 0;
  std::map<GameId, CloudStatus> cloud_status_;
  // Cloud requests wait for the console to confirm the game's title is stopped:
  // asked once the activity check is queued, answered by a newer serial.
  struct HeldCloudRequest {
    CloudRequest request;
    bool asked = false;
    std::uint64_t after = 0;
  };
  std::vector<HeldCloudRequest> cloud_held_;
  // The Saves tab: its last result restored once; the quick check once per run and when the tab
  // opens (at most every two minutes); jobs wait for the console to say which titles run.
  std::optional<steam::CloudLocation> CloudLocationFor(GameId id, bool for_job) const;
  void QueueSavesJob(steam::CloudJobKind kind);
  struct PendingJob {
    steam::CloudJobKind kind = steam::CloudJobKind::kNone;
    std::vector<GameId> ids;
    bool asked = false;
    std::uint64_t after = 0;
    std::chrono::steady_clock::time_point deadline{};
  };
  std::optional<PendingJob> pending_job_;
  bool cloud_restored_ = false;
  bool startup_quick_check_ = false;
  std::optional<std::chrono::steady_clock::time_point> last_quick_check_;
  // Games whose title may have been running when a job started: they sync once it stops.
  std::set<GameId> after_play_;
  std::set<GameId> after_play_projected_;
  std::optional<std::chrono::steady_clock::time_point> after_play_checked_;
  // Games whose next Cloud result answers the player (a dialog may open); others are background.
  // The serial after which the game's next settled report answers the player's request.
  static constexpr std::uint64_t kNotDispatched = ~std::uint64_t{0};
  // How long a Cloud job waits for the console to say which games are stopped.
  static constexpr std::chrono::seconds kJobActivityWait{45};
  std::map<GameId, std::uint64_t> cloud_foreground_;
  // Games the job gate refuses (sessions Z3CloudPlay knows of), read on the Cloud worker.
  std::mutex gate_mutex_;
  std::set<GameId> gate_blocked_;
  LiveSavesJob saves_job_;
  std::map<GameId, std::uint64_t> save_sizes_;
  std::optional<std::pair<GameId, runtime::LaunchState>> last_launch_;
  // A game whose title may be running: Cloud must not touch its saves.
  bool SessionActive(GameId id) const;
  std::chrono::steady_clock::time_point volume_refreshed_at_{};
  std::optional<GameId> requested_detail_;
  std::uint64_t requested_generation_ = 0;
  std::unique_ptr<steam::SteamService> service_;
  LibraryService* library_service_ = nullptr;
  std::unique_ptr<app::LibraryController> library_;
  std::unique_ptr<app::PersistenceController> persistence_;
  AuthState established_auth_ = AuthState::SignedOut;
  bool retry_saved_session_ = false;
  std::chrono::steady_clock::time_point retry_saved_session_at_{};
  steam::RequestId request_ = 0;
  std::uint64_t generation_ = 0;
  Phase phase_ = Phase::Idle;
  bool begin_after_clear_ = false;
  // A QR sign-in for an account that is already signed in: its library and organization stay, and
  // a cancel or failure goes back to the saved sign-in.
  bool reauthenticating_ = false;
  std::array<std::int64_t, 9> tick_us_{};
};
}
