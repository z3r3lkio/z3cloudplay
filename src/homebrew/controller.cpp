#include "homebrew/controller.hpp"
#include "diagnostics/redaction.h"
#include "steam/install_volumes.h"
#include <chrono>

namespace z3cloudplay::homebrew {
Controller::Controller(Simulation& model, steam::AuthBackend& backend, SessionRepository& repository,
                       LibraryService* library_service, app::PersistenceService* persistence_service,
                       steam::AppDetailsService* app_details, steam::InstallService* installs,
                       steam::CloudService* cloud, RuntimeHost* runtime)
    : model_(model), app_details_(app_details), installs_(installs), cloud_(cloud), runtime_(runtime),
      library_service_(library_service) {
  if (model_.state().simulation_mode) return;
  if(library_service_) library_=std::make_unique<app::LibraryController>(model_,*library_service_);
  service_ = std::make_unique<steam::SteamService>(backend,repository);
  phase_ = Phase::Restore;
  Update(AuthState::Restoring,{},true);
  if(persistence_service) persistence_=std::make_unique<app::PersistenceController>(model_,*persistence_service);
  request_ = service_->RestoreSession();
  if (cloud_)
    // On the Cloud worker, just before a job checks a game: never while its title may run.
    cloud_->SetGate([this](GameId id) {
      {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        if (gate_blocked_.count(id)) return false;
      }
      return !runtime_ || runtime_->ActivityNow(id, std::chrono::seconds(5)) == runtime::NativeActivity::kStopped;
    });
}
Controller::~Controller() { Shutdown(); }
void Controller::Shutdown() {
  if (!service_) return;
  // Preserve a confirmed sign-out queued by the last input frame before exit.
  for (const auto action : model_.TakeServiceActions()) {
    if ((action == ServiceAction::SignOut || action == ServiceAction::SwitchAccount) &&
        phase_!=Phase::ClearLibrary && phase_!=Phase::ClearOrganization && phase_!=Phase::SignOut) SignOut(false);
  }
  begin_after_clear_=false;
  if(library_service_) {
    library_service_->Shutdown();
    library_->Tick();AdvanceClear();
  }
  if(persistence_) { persistence_->FlushAndShutdown();AdvanceOrganizationClear(); }
  if(app_details_) app_details_->Shutdown();
  if(installs_) installs_->Shutdown();
  if(cloud_) cloud_->Shutdown();
  if(runtime_) runtime_->Shutdown();
  service_.reset();
  persistence_.reset();
  library_.reset();
  phase_ = Phase::Idle;
  Update(AuthState::SignedOut,{},true);
}
void Controller::Update(AuthState state,const std::string& error,bool clear,const std::string& challenge) {
  AccountUpdate update;
  update.authentication = state;
  update.id = clear ? std::string{} : model_.state().account.id;
  update.display_name = clear ? std::string{} : model_.state().account.display_name;
  update.error = error; update.challenge_url = challenge; update.clear_library = clear;
  model_.UpdateAccount(std::move(update));
}
void Controller::Begin() {
  if (retry_saved_session_) {
    if (phase_ == Phase::Idle) Restore();
    return;
  }
  if (phase_ == Phase::SignOut || phase_==Phase::ClearLibrary || phase_==Phase::ClearOrganization) { begin_after_clear_ = true; return; }
  if(!model_.state().account.id.empty() && established_auth_==AuthState::Offline) { Restore();return; }
  if(!model_.state().account.id.empty() && established_auth_==AuthState::SignedIn) { SignOut(true);return; }
  if(library_) library_->Reset();
  if(persistence_) persistence_->Reset();
  phase_ = Phase::SignIn;
  request_ = service_->BeginQrSignIn();
  Update(AuthState::Waiting,{},true);
}
void Controller::Reauthenticate() {
  if (phase_ != Phase::Idle || model_.state().account.id.empty()) return;
  reauthenticating_ = true;
  phase_ = Phase::SignIn;
  request_ = service_->BeginQrSignIn();
  Update(AuthState::Waiting);
}
void Controller::RestoreAfterUnlock() {
  if (!service_) return;
  // Existing SteamService worker serializes replacement/cancel requests.
  Restore();
}
void Controller::Restore() {
  if(library_) library_->Reset();
  phase_=Phase::Restore;request_=service_->RestoreSession();
  Update(AuthState::Restoring);
}
void Controller::SignOut(bool switch_account) {
  if(phase_==Phase::ClearLibrary || phase_==Phase::ClearOrganization || phase_==Phase::SignOut) return;
  retry_saved_session_ = false;
  begin_after_clear_ = switch_account;
  request_ = 0;
  if(library_) { phase_=Phase::ClearLibrary;library_->Clear(); }
  else BeginOrganizationClear();
  Update(AuthState::SigningOut);
}
void Controller::AdvanceClear() {
  if(phase_!=Phase::ClearLibrary || !library_) return;
  const auto result=library_->TakeClearResult();
  if(!result) return;
  if(*result==steam::LibraryError::kNone) {
    BeginOrganizationClear();
  } else {
    phase_=Phase::Idle;begin_after_clear_=false;
    Update(model_.state().account.id.empty()?AuthState::Failed:established_auth_,
           "Sign-out failed. Your saved library remains. Retry sign-out in Settings.");
  }
}
void Controller::BeginOrganizationClear() {
  if(persistence_) {phase_=Phase::ClearOrganization;persistence_->Clear();}
  else {phase_=Phase::SignOut;service_->SignOut();}
}
void Controller::AdvanceOrganizationClear() {
  if(phase_!=Phase::ClearOrganization || !persistence_) return;
  const auto result=persistence_->TakeClearResult();
  if(!result) return;
  if(*result==StorageError::kNone) {phase_=Phase::SignOut;service_->SignOut();}
  else {
    phase_=Phase::Idle;begin_after_clear_=false;
    Update(model_.state().account.id.empty()?AuthState::Failed:established_auth_,
           "Sign-out failed. Your saved organization remains. Retry sign-out in Settings.");
  }
}
void Controller::Tick() {
  if (!service_) return;
  model_.SetWallClock(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  for (const auto action : model_.TakeServiceActions()) {
    switch (action) {
      case ServiceAction::BeginQr: Begin(); break;
      case ServiceAction::Reauthenticate: Reauthenticate(); break;
      case ServiceAction::CancelQr:
        // Stop automatic retries while preserving the need to restore the
        // saved account when the user chooses Connect again.
        retry_saved_session_at_ = std::chrono::steady_clock::time_point::max();
        if (phase_ == Phase::SignIn && reauthenticating_) {
          // Back to the saved sign-in; nothing was cleared.
          service_->Cancel(request_);
          reauthenticating_ = false;
          Restore();
          break;
        }
        if (phase_ == Phase::SignIn || phase_ == Phase::Restore) {
          service_->Cancel(request_);
          if(library_) library_->Reset();
          if(persistence_) persistence_->Reset();
          Update(AuthState::Cancelled,{},!retry_saved_session_);
        }
        break;
      case ServiceAction::SignOut: SignOut(false); break;
      case ServiceAction::SwitchAccount: SignOut(true); break;
      case ServiceAction::RefreshLibrary:
        if (phase_ == Phase::Idle && !model_.state().account.id.empty()) {
          if(library_) {
            if(established_auth_==AuthState::Offline || library_->NeedsRenewal()) Restore();
            else library_->Refresh();
          } else model_.UpdateLibrary({{},false,"Library sync is unavailable. Try again later.",
                                        model_.state().library_offline,model_.state().account.cache_available,{}});
        }
        break;
    }
  }
  if (retry_saved_session_ && phase_ == Phase::Idle &&
      std::chrono::steady_clock::now() >= retry_saved_session_at_) Restore();
  auto mark = std::chrono::steady_clock::now();
  const auto lap = [&](std::size_t index) {
    const auto now = std::chrono::steady_clock::now();
    tick_us_[index] = std::chrono::duration_cast<std::chrono::microseconds>(now - mark).count();
    mark = now;
  };
  for (const auto& event : service_->Pump(16)) Apply(event);
  lap(0);
  if(library_) { library_->Tick();AdvanceClear(); }
  lap(1);
  if(persistence_) { persistence_->Tick();AdvanceOrganizationClear(); }
  lap(2);
  TickAppDetails();
  lap(3);
  TickStoreDetails();
  lap(4);
  TickInstalls();
  lap(5);
  TickCloud();
  lap(6);
  TickRuntime();
  lap(7);
  TickUninstalls();
  lap(8);
}
std::string Controller::LastTickProfile() const {
  static const char* const kNames[] = {"auth", "library", "persistence", "details", "store", "installs", "cloud", "runtime",
                                       "uninstalls"};
  std::string out;
  for (std::size_t index = 0; index < tick_us_.size(); ++index)
    if (tick_us_[index] >= 1000) out += std::string(out.empty() ? "" : " ") + kNames[index] + "=" + std::to_string(tick_us_[index] / 1000);
  return out;
}
bool Controller::SessionActive(GameId id) const {
  if (!runtime_) return false;
  const auto snapshot_shared = runtime_->Shared();
  const auto& snapshot = *snapshot_shared;
  // Until the runtime has started, a session pending from before a restart is
  // not known yet: no game's saves move.
  if (snapshot.status == RuntimeHostStatus::kStarting) return true;
  if (play_queued_.count(id)) return true;
  if (!snapshot.launch || snapshot.launch->app_id != id) return false;
  using runtime::LaunchState;
  const auto state = snapshot.launch->state;
  return state == LaunchState::kReadyToHandoff || state == LaunchState::kLaunching || state == LaunchState::kRunning;
}
std::optional<steam::CloudLocation> Controller::CloudLocationFor(GameId id, bool for_job) const {
  if (saves_root_.empty()) return std::nullopt;
  steam::CloudLocation location{id, {}, saves_root_ + "/" + std::to_string(id)};
  if (const auto root = game_roots_.find(id); root != game_roots_.end()) {
    location.game_root = root->second;
    // A prepared game keeps its Windows-folder saves in its private bottle, also while its
    // PS5 title waits for registration: the bottle is complete and the game runs from Home.
    if (runtime_) {
      const auto installs_shared = runtime_->Shared();
      for (const auto& view : installs_shared->installs)
        if (view.app_id == id && (view.state == runtime::PreparationState::kReady || view.awaiting_registration))
          location.prefix_root = view.prefix_root;
    }
    // Not set up yet (or still being prepared): there is no bottle the game reads, so Cloud only
    // checks. Saves written anywhere else were never seen by the game (Mullet Mad Jack, #72).
    if (location.prefix_root.empty()) location.check_only = true;
    return location;
  }
  // An uninstalled game's kept bottle: no game folder; a job only checks it.
  if (runtime_) {
    const auto kept_bottles_shared = runtime_->Shared();
    for (const auto& kept : kept_bottles_shared->kept_bottles)
      if (kept.app_id == id && !kept.data_root.empty()) {
        location.prefix_root = kept.data_root + "/library/prefixes/game";
        location.check_only = for_job;
        return location;
      }
  }
  return std::nullopt;
}

void Controller::QueueSavesJob(steam::CloudJobKind kind) {
  if (!cloud_ || saves_root_.empty()) return;
  if (cloud_->Snapshot().job.kind != steam::CloudJobKind::kNone) return;
  if (pending_job_ && (pending_job_->kind == kind || kind == steam::CloudJobKind::kQuickCheck)) return;
  PendingJob job;
  job.kind = kind;
  for (const auto& [id, root] : game_roots_) job.ids.push_back(id);
  if (runtime_) {
    const auto kept_bottles_shared = runtime_->Shared();
    for (const auto& kept : kept_bottles_shared->kept_bottles)
      if (!game_roots_.count(kept.app_id)) job.ids.push_back(kept.app_id);
  }
  job.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  pending_job_ = std::move(job);
  if (kind == steam::CloudJobKind::kQuickCheck) last_quick_check_ = std::chrono::steady_clock::now();
  if (runtime_) runtime_->MeasureSaves();
}

void Controller::TickCloud() {
  if (!cloud_) return;
  if (!cloud_restored_ && !saves_root_.empty()) {
    // The last result of every game, so the Saves tab shows them at once.
    cloud_->Restore(saves_root_);
    cloud_restored_ = true;
  }
  {
    // Games whose sessions Z3CloudPlay knows about, for the job gate on the Cloud worker.
    std::set<GameId> blocked;
    for (const auto& [id, root] : game_roots_)
      if (SessionActive(id)) blocked.insert(id);
    std::lock_guard<std::mutex> lock(gate_mutex_);
    gate_blocked_ = std::move(blocked);
  }
  for (const auto id : cloud_->TakeDeferred()) after_play_.insert(id);
  const auto now = std::chrono::steady_clock::now();
  const bool signed_in = model_.state().account.authentication == AuthState::SignedIn;
  auto requests = model_.TakeCloudRequests();
  std::vector<CloudRequest> transfers;
  for (const auto& request : requests) {
    switch (request.kind) {
      case CloudRequestKind::QuickCheck:
        // Opening Saves checks what may have changed, at most every two minutes.
        if (signed_in && (!last_quick_check_ || now - *last_quick_check_ >= std::chrono::minutes(2)))
          QueueSavesJob(steam::CloudJobKind::kQuickCheck);
        else if (runtime_)
          runtime_->MeasureSaves();
        break;
      case CloudRequestKind::SyncAll:
        if (signed_in) QueueSavesJob(steam::CloudJobKind::kSyncAll);
        break;
      case CloudRequestKind::StopJob:
        pending_job_.reset();
        cloud_->StopJob();
        break;
      case CloudRequestKind::RemoveLeftover:
        // The kept bottle and the game's sync state; no PS5 title is involved.
        if (runtime_) runtime_->DeleteKeptBottle(request.game_id);
        if (!saves_root_.empty())
          cloud_->Forget({request.game_id, {}, saves_root_ + "/" + std::to_string(request.game_id)});
        after_play_.erase(request.game_id);
        break;
      default:
        cloud_foreground_[request.game_id] = kNotDispatched;
        transfers.push_back(request);
        break;
    }
  }
  // Once per run, after sign-in and once the runtime knows the games: catch up with
  // sessions started from the PS5 Home screen since Z3CloudPlay last ran.
  const bool auto_sync = model_.state().cloud_saves_auto;
  if (!startup_quick_check_ && auto_sync && signed_in && !saves_root_.empty() &&
      (!runtime_ || runtime_->Shared()->status != RuntimeHostStatus::kStarting)) {
    startup_quick_check_ = true;
    QueueSavesJob(steam::CloudJobKind::kQuickCheck);
  }
  // A session that just ended syncs its saves, as Steam does after play.
  for (auto& [id, issued] : sync_before_play_)
    if (!issued) {
      transfers.push_back({CloudRequestKind::Sync, id});
      cloud_foreground_[id] = kNotDispatched;
      issued = true;
    }
  for (const auto id : uninstalls_.TakeSyncRequests()) {
    transfers.push_back({CloudRequestKind::Sync, id});
    cloud_foreground_[id] = kNotDispatched;
  }
  for (auto it = sync_after_session_.begin(); it != sync_after_session_.end();) {
    if (!auto_sync) {
      it = sync_after_session_.erase(it);  // automatic sync is off: the player syncs from Saves
    } else if (game_roots_.count(*it) && !saves_root_.empty()) {
      transfers.push_back({CloudRequestKind::Sync, *it});
      it = sync_after_session_.erase(it);
    } else {
      ++it;
    }
  }
  // Games that may have been running when a job started sync once they stop; asked again
  // every half minute.
  if (!auto_sync) after_play_.clear();
  if (!after_play_.empty() && (!after_play_checked_ || now - *after_play_checked_ >= std::chrono::seconds(30))) {
    after_play_checked_ = now;
    for (const auto id : after_play_)
      if (std::none_of(cloud_held_.begin(), cloud_held_.end(), [id](const HeldCloudRequest& held) { return held.request.game_id == id; }))
        transfers.push_back({CloudRequestKind::Sync, id});
  }
  for (const auto& request : transfers) cloud_held_.push_back({request, false, 0});
  // Saves move only once the console confirms the game's own title is stopped,
  // whoever started it (Z3CloudPlay or the PS5 Home screen). Unknown counts as running.
  std::vector<CloudRequest> confirmed;
  if (!runtime_) {
    for (const auto& held : cloud_held_) confirmed.push_back(held.request);
    cloud_held_.clear();
  } else {
    const auto runtime_shared = runtime_->Shared();
    const auto& runtime = *runtime_shared;
    std::vector<HeldCloudRequest> waiting;
    for (auto& held : cloud_held_) {
      const auto id = held.request.game_id;
      if (runtime.status == RuntimeHostStatus::kStarting) { waiting.push_back(held); continue; }
      if (!held.asked) {
        held.asked = true;
        held.after = runtime.activity_serial;
        runtime_->CheckActivity(id);
        waiting.push_back(held);
        continue;
      }
      const auto answer = runtime.activity.find(id);
      if (answer == runtime.activity.end() || answer->second.second <= held.after) { waiting.push_back(held); continue; }
      if (answer->second.first == runtime::NativeActivity::kStopped && !SessionActive(id)) {
        confirmed.push_back(held.request);
        continue;
      }
      // Refused: the title runs, or its state is unknown.
      if (cloud_foreground_.erase(id)) model_.NotifyCloudRefused(id);
      if (const auto stopped = uninstalls_.CloudRefused(id); stopped.kind == UninstallAction::Kind::kStop)
        StopUninstall(id, stopped.reason);
      if (sync_before_play_.erase(id)) {
        play_notes_[id] = "The game's PS5 title may still be running. Close it, then press Play.";
        ++play_notes_revision_;
      }
    }
    cloud_held_ = std::move(waiting);
  }
  for (const auto& request : confirmed) {
    after_play_.erase(request.game_id);
    const auto location = CloudLocationFor(request.game_id, false);
    if (!location) {
      // Nothing is there to sync: the request is answered as such.
      if (cloud_foreground_.erase(request.game_id)) model_.NotifyCloudAnswered(request.game_id, CloudStatus::Unsupported);
      // A pre-uninstall check that cannot run goes straight on: nothing is there to sync.
      uninstalls_.CloudSkipped(request.game_id);
      // A pre-play check that cannot run must not leave Play waiting.
      if (sync_before_play_.erase(request.game_id)) {
        play_notes_.erase(request.game_id);
        ++play_notes_revision_;
        plays_ready_.push_back(request.game_id);
      }
      continue;
    }
    // Busy from now on, before Cloud reports it, so Play cannot slip in.
    cloud_busy_.insert(request.game_id);
    if (request.kind == CloudRequestKind::Sync) uninstalls_.SyncDispatched(request.game_id);
    std::uint64_t serial = 0;
    switch (request.kind) {
      case CloudRequestKind::Sync: serial = cloud_->Sync(*location); break;
      case CloudRequestKind::UseLocal: serial = cloud_->Resolve(*location, steam::CloudResolution::kUseLocal, true); break;
      case CloudRequestKind::UseCloud: serial = cloud_->Resolve(*location, steam::CloudResolution::kUseCloud, true); break;
      case CloudRequestKind::ConfirmAccount: serial = cloud_->Resolve(*location, steam::CloudResolution::kAutomatic, true); break;
      case CloudRequestKind::RemoveLocal: serial = cloud_->RemoveLocal(*location); break;
      default: break;
    }
    // The answer is the game's first settled report after this serial.
    if (const auto foreground = cloud_foreground_.find(request.game_id); foreground != cloud_foreground_.end()) {
      if (serial == 0) {
        cloud_foreground_.erase(foreground);
        model_.NotifyCloudRefused(request.game_id);
      } else {
        foreground->second = serial;
      }
    }
  }
  // A job asks the console about every game first; games whose title may run wait.
  if (pending_job_) {
    auto& job = *pending_job_;
    bool answered = true;
    if (runtime_) {
      const auto runtime_shared = runtime_->Shared();
      const auto& runtime = *runtime_shared;
      if (runtime.status == RuntimeHostStatus::kStarting) {
        answered = false;
      } else if (!job.asked) {
        job.asked = true;
        job.after = runtime.activity_serial;
        // The console answers for every game in turn, and right after start-up that takes longer
        // than 8 s. A game left unanswered is synced on its own afterwards, two Steam logons each,
        // so wait long enough for the answers instead (#51).
        job.deadline = now + kJobActivityWait;
        for (const auto id : job.ids) runtime_->CheckActivity(id);
        answered = false;
      } else {
        for (const auto id : job.ids) {
          const auto answer = runtime.activity.find(id);
          if (answer == runtime.activity.end() || answer->second.second <= job.after) answered = false;
        }
      }
      if (!answered && now >= job.deadline && job.asked) answered = true;  // unanswered ones count as running
      if (answered) {
        std::vector<GameId> stopped;
        for (const auto id : job.ids) {
          const auto answer = runtime.activity.find(id);
          const bool known = answer != runtime.activity.end() && answer->second.second > job.after;
          if (known && answer->second.first == runtime::NativeActivity::kStopped && !SessionActive(id)) stopped.push_back(id);
          else after_play_.insert(id);
        }
        job.ids = std::move(stopped);
      }
    }
    if (answered) {
      std::vector<steam::CloudLocation> locations;
      for (const auto id : job.ids)
        if (const auto location = CloudLocationFor(id, true)) locations.push_back(*location);
      cloud_->StartJob(job.kind, std::move(locations));
      pending_job_.reset();
    }
  }
  const auto snapshot = cloud_->Snapshot();
  LiveSavesJob job{snapshot.job.kind == steam::CloudJobKind::kSyncAll     ? SavesJobKind::SyncAll
                   : snapshot.job.kind == steam::CloudJobKind::kQuickCheck ? SavesJobKind::QuickCheck
                                                                           : SavesJobKind::None,
                   snapshot.job.done, snapshot.job.total, snapshot.job.current, snapshot.job.stopping};
  if (job.kind != saves_job_.kind || job.done != saves_job_.done || job.total != saves_job_.total ||
      job.current != saves_job_.current || job.stopping != saves_job_.stopping) {
    saves_job_ = job;
    model_.UpdateSavesJob(job);
  }
  if (runtime_) {
    const auto sizes = runtime_->Shared()->save_bytes;
    std::map<GameId, std::uint64_t> projected(sizes.begin(), sizes.end());
    if (projected != save_sizes_) {
      save_sizes_ = projected;
      model_.UpdateSaveSizes(std::move(projected));
    }
  }
  if (snapshot.revision == cloud_revision_ && after_play_ == after_play_projected_) return;
  cloud_revision_ = snapshot.revision;
  after_play_projected_ = after_play_;
  // Settle Play requests whose Cloud check has finished. Sync marks a game as
  // checking at once, so a finished state seen here is the check's own result.
  const auto settle_uninstall = [this](GameId id, CloudStatus status) {
    const bool waiting = uninstalls_.WaitingForCloud(id);
    const auto action = uninstalls_.CloudSettled(id, status);
    if (action.kind == UninstallAction::Kind::kStop) StopUninstall(id, action.reason);
    else if (waiting && !uninstalls_.WaitingForCloud(id)) ++play_notes_revision_;
  };
  const auto settle = [this, &settle_uninstall](GameId id, CloudStatus status) {
    settle_uninstall(id, status);
    // A choice made after Play stopped for it: the note and the second-Play escape no longer apply.
    if ((status == CloudStatus::Synced || status == CloudStatus::UpToDate) && play_without_sync_.erase(id)) {
      play_notes_.erase(id);
      ++play_notes_revision_;
    }
    const auto pending = sync_before_play_.find(id);
    if (pending == sync_before_play_.end() || !pending->second) return;
    if (status == CloudStatus::Checking || status == CloudStatus::Syncing) return;
    sync_before_play_.erase(pending);
    ++play_notes_revision_;
    switch (status) {
      case CloudStatus::Synced:
      case CloudStatus::UpToDate:
      case CloudStatus::Unsupported:
        play_notes_.erase(id);
        plays_ready_.push_back(id);
        break;
      case CloudStatus::Conflict:
      case CloudStatus::NeedsAccount:
        // Like Steam's "Play anyway": a second Play starts on this console's saves and changes nothing in the Cloud.
        play_notes_[id] = "Choose which Cloud saves to keep, or press Play again to start without syncing.";
        play_without_sync_.insert(id);
        break;
      default:
        play_notes_[id] = "Cloud saves could not be checked. Press Play again to start without syncing.";
        play_without_sync_.insert(id);
        break;
    }
  };
  std::vector<LiveCloudGame> games;
  std::vector<std::pair<GameId, CloudStatus>> answered;
  for (const auto& game : snapshot.games) {
    CloudStatus status = CloudStatus::None;
    switch (game.report.state) {
      case steam::CloudState::kIdle: status = CloudStatus::None; break;
      case steam::CloudState::kChecking: status = CloudStatus::Checking; break;
      case steam::CloudState::kSyncing: status = CloudStatus::Syncing; break;
      case steam::CloudState::kSynced: status = CloudStatus::Synced; break;
      case steam::CloudState::kUpToDate: status = CloudStatus::UpToDate; break;
      case steam::CloudState::kConflict: status = CloudStatus::Conflict; break;
      case steam::CloudState::kNeedsAccount: status = CloudStatus::NeedsAccount; break;
      case steam::CloudState::kUnsupported: status = CloudStatus::Unsupported; break;
      case steam::CloudState::kFailed: status = CloudStatus::Failed; break;
      case steam::CloudState::kWaiting: status = CloudStatus::Waiting; break;
    }
    const auto& report = game.report;
    if (status == CloudStatus::Checking || status == CloudStatus::Syncing) cloud_busy_.insert(game.id);
    else cloud_busy_.erase(game.id);
    cloud_status_[game.id] = status;
    settle(game.id, status);
    LiveCloudGame live{game.id, status, report.message, report.local_files, report.remote_files, report.local_bytes,
                       report.remote_bytes, static_cast<std::int64_t>(report.local_latest),
                       static_cast<std::int64_t>(report.remote_latest)};
    live.first_sync = report.first_sync;
    live.first_overwrite = report.first_overwrite;
    live.last_sync = report.last_sync;
    live.queued = report.queued;
    live.moved_bytes = report.moved_bytes;
    live.total_bytes = report.total_bytes;
    live.downloads = report.downloads;
    live.uploads = report.uploads;
    live.after_play = after_play_.count(game.id) > 0;
    // A result the player asked for may open a dialog; one from a background check never does.
    const auto foreground = cloud_foreground_.find(game.id);
    live.background = foreground == cloud_foreground_.end();
    if (foreground != cloud_foreground_.end() && foreground->second != kNotDispatched && report.serial > foreground->second &&
        status != CloudStatus::Checking && status != CloudStatus::Syncing && status != CloudStatus::None) {
      cloud_foreground_.erase(foreground);
      answered.emplace_back(game.id, status);
    }
    games.push_back(std::move(live));
  }
  model_.UpdateCloud(games);
  for (const auto& [id, status] : answered) model_.NotifyCloudAnswered(id, status);
}
// Per-game runtime: queue Prepare with the Steam install's location, queue Play,
// and project the runtime host's state for installed games. The worker checks a
// Play request; this thread, the application thread, then starts the title.
void Controller::TickRuntime() {
  if (!runtime_) return;
  {
    // The name each installed game's PS5 title should carry.
    std::map<std::uint32_t, std::string> names;
    for (const auto& [id, root] : game_roots_)
      for (const auto& game : model_.state().games)
        if (game.id == id && !game.title.empty())
          names[id] = model_.state().title_store_suffix ? game.title + " (Steam)" : game.title;
    runtime_->SetTitleNames(std::move(names));
  }
  for (const auto id : model_.TakeRuntimePrepareRequests()) {
    if (!game_roots_.count(id)) continue;
    // Without Steam's launch options yet, ask Steam first; TickAppDetails submits it.
    if (app_details_ && !launch_options_.count(id) && !launch_options_reason_.count(id) &&
        model_.state().account.authentication == AuthState::SignedIn) {
      prepare_after_details_.insert(id);
      app_details_->Request(id, model_.state().account_generation);
      continue;
    }
    SubmitPrepare(id);
  }
  if (model_.TakeRuntimeCleanupRequest()) runtime_->CleanUpRequests();
  for (const auto id : model_.TakeKeptDeleteRequests()) runtime_->DeleteKeptBottle(id);
  for (const auto id : model_.TakeActivityRequests()) runtime_->CheckActivity(id);
  for (const auto id : model_.TakeProgramScanRequests())
    if (const auto root = game_roots_.find(id); root != game_roots_.end()) runtime_->ScanPrograms(id, root->second);
  for (const auto& [id, kind] : model_.TakeMaintenanceRequests()) {
    if (kind == MaintenanceKind::RecreateBottle) {
      if (recreate_.count(id)) continue;
      recreate_[id] = {runtime_->Shared()->removals_handled, false, false, {}, {}};
      recreate_results_.erase(id);
      runtime_->Remove(id, false);
      ++play_notes_revision_;
      continue;
    }
    recreate_results_.erase(id);  // the sheet shows the newest request
    ++play_notes_revision_;
    runtime_->Maintain(id, kind == MaintenanceKind::ClearShaderCache ? RuntimeMaintenance::kClearShaderCache
                         : kind == MaintenanceKind::Repair ? RuntimeMaintenance::kRepair
                         : kind == MaintenanceKind::BackupSaves ? RuntimeMaintenance::kBackupSaves
                                                                : RuntimeMaintenance::kApplySettings);
  }
  {
    const auto current_shared = runtime_->Shared();
    const auto& current = *current_shared;
    if (current.revision != kept_revision_) {
      kept_revision_ = current.revision;
      std::vector<LiveKeptBottle> kept;
      for (const auto& bottle : current.kept_bottles) kept.push_back({bottle.app_id, bottle.name, bottle.kept_at, bottle.bytes});
      if (kept != kept_projected_) { kept_projected_ = kept; model_.UpdateKeptBottles(std::move(kept)); }
    }
  }
  auto plays = model_.TakeRuntimePlayRequests();
  {
    const auto prepared_shared = runtime_->Shared();
    const auto& prepared = *prepared_shared;
    const bool usable = prepared.status == RuntimeHostStatus::kReady && prepared.kit_available;
    const auto record_of = [&prepared](GameId id) -> const RuntimeInstallView* {
      for (const auto& view : prepared.installs) if (view.app_id == id) return &view;
      return nullptr;
    };
    const auto ready = [](const RuntimeInstallView* view) {
      return view && view->state == runtime::PreparationState::kReady && !view->recovery_pending &&
             view->title_current && view->admission == "ok";
    };
    const auto refusal_of = [&prepared](GameId id) {
      const auto found = prepared.refusals.find(id);
      return found == prepared.refusals.end() ? std::string{} : found->second;
    };
    // Play on a game that was never prepared prepares it first; on one whose title needs an
    // Update or a Repair, it updates or repairs it first. Both then start the game.
    const auto needs_work = [](const RuntimeInstallView* view) {
      return view && view->state == runtime::PreparationState::kReady && !view->recovery_pending &&
             (!view->title_current || view->admission != "ok");
    };
    // A record left before Ready by an earlier session, with no journal to resume, is what the
    // game page calls "Not prepared": Play prepares it too, instead of asking the launcher.
    const auto unprepared = [](const RuntimeInstallView* view) {
      using runtime::PreparationState;
      return !view || ((view->state == PreparationState::kDownloaded || view->state == PreparationState::kPreparing ||
                        view->state == PreparationState::kRegistering) &&
                       !view->recovery_pending && !view->awaiting_registration);
    };
    std::vector<GameId> direct;
    for (const auto id : plays) {
      if (usable && game_roots_.count(id) && (unprepared(record_of(id)) || needs_work(record_of(id))) &&
          !play_after_prepare_.count(id)) {
        auto& pending = play_after_prepare_[id];
        if (const auto* view = record_of(id); needs_work(view))
          pending.words = !view->title_current ? "Updating runtime…" : "Repairing runtime…";
        auto_prepare_.erase(id);
        ++play_notes_revision_;
      } else if (!play_after_prepare_.count(id)) {
        direct.push_back(id);
      }
    }
    plays = std::move(direct);
    // Finished downloads are prepared one at a time, never over a game that already has a record.
    if (prepared.status != RuntimeHostStatus::kStarting)
      for (auto it = auto_prepare_.begin(); it != auto_prepare_.end();) {
        if (!usable || !game_roots_.count(*it) || record_of(*it)) { it = auto_prepare_.erase(it); continue; }
        if (prepared.preparing_app) break;
        play_after_prepare_.erase(*it);
        model_.RequestRuntimePrepare(*it);
        auto_prepare_.erase(it);
        break;
      }
    // A title still waiting for the PS5 when Z3CloudPlay started is checked once: the
    // player left Z3CloudPlay since, so ShadowMountPlus has usually registered it. The
    // check only asks the console.
    if (usable && !startup_awaiting_known_) {
      startup_awaiting_known_ = true;
      for (const auto& view : prepared.installs)
        if (view.awaiting_registration) startup_awaiting_.insert(view.app_id);
    }
    for (auto it = startup_awaiting_.begin(); it != startup_awaiting_.end();) {
      if (prepared.preparing_app) break;
      if (!game_roots_.count(*it)) { ++it; continue; }
      const auto* record = record_of(*it);
      if (record && record->awaiting_registration) SubmitPrepare(*it, false);
      it = startup_awaiting_.erase(it);
      break;  // one at a time, after the worker takes it
    }
    // A preparation the player started that ends waiting for the PS5: the game is
    // ready to play once Z3CloudPlay quits to Home, so the model offers that.
    for (auto it = ready_watch_.begin(); it != ready_watch_.end();) {
      const auto id = it->first;
      if (prepared.preparing_app == id) { it->second.seen_preparing = true; ++it; continue; }
      // A preparation still queued in the runtime is not lost, however long it waits.
      const bool unseen_too_long = !it->second.seen_preparing && !prepared.queued_prepares.count(id) &&
          std::chrono::steady_clock::now() - it->second.submitted_at > std::chrono::seconds(15);
      if (!it->second.seen_preparing && !unseen_too_long) { ++it; continue; }
      const auto* record = record_of(id);
      // Announced once the model has the game's new state (after the projection below).
      if (it->second.seen_preparing && record && record->awaiting_registration) {
        ready_notices_.push_back(id);
        // The bottle is complete now and the game will first start from Home, maybe with Z3CloudPlay
        // closed: bring its Cloud saves in while Z3CloudPlay is still open (#50). With automatic sync
        // off this does nothing (TickCloud drops it).
        sync_after_session_.insert(id);
      }
      it = ready_watch_.erase(it);
    }
    // Recreate bottle: once the title is removed, prepare the game again; it ends when that
    // preparation does (Ready, or waiting for the PS5 to register the new title).
    for (auto it = recreate_.begin(); it != recreate_.end();) {
      const auto id = it->first;
      auto& pending = it->second;
      const auto finish = [&](bool ok, std::string message) {
        recreate_results_[id] = {ok, std::move(message)};
        ++play_notes_revision_;
        it = recreate_.erase(it);
      };
      if (!pending.prepare_submitted) {
        if (prepared.removals_handled <= pending.removals_before || prepared.removing_app == id) { ++it; continue; }
        if (const auto failure = prepared.removal_failures.find(id); failure != prepared.removal_failures.end()) {
          finish(false, failure->second);
          continue;
        }
        if (record_of(id)) { finish(false, "The PS5 title could not be removed."); continue; }
        if (prepared.preparing_app) { ++it; continue; }
        pending.prepare_submitted = true;
        pending.refusal = refusal_of(id);
        pending.submitted_at = std::chrono::steady_clock::now();
        SubmitPrepare(id);
        ++it;
        continue;
      }
      if (prepared.preparing_app == id) { pending.seen_preparing = true; ++it; continue; }
      const bool unseen_too_long = !pending.seen_preparing && !prepared.queued_prepares.count(id) &&
          std::chrono::steady_clock::now() - pending.submitted_at > std::chrono::seconds(15);
      if (!pending.seen_preparing && !unseen_too_long) { ++it; continue; }
      const auto* record = record_of(id);
      if (record && (ready(record) || record->awaiting_registration))
        finish(true, record->awaiting_registration ? "Bottle recreated. The PS5 adds the game to Home when Z3CloudPlay quits."
                                                   : "Bottle recreated with the saves and settings brought back.");
      else
        finish(false, refusal_of(id).empty() ? "The game could not be prepared again; press Prepare on the game page."
                                              : refusal_of(id));
    }
    for (auto it = play_after_prepare_.begin(); it != play_after_prepare_.end();) {
      const auto id = it->first;
      auto& pending = it->second;
      if (!pending.queued) {
        if (prepared.preparing_app && *prepared.preparing_app != id) { ++it; continue; }
        pending.queued = true;
        pending.refusal = refusal_of(id);
        pending.prepares_handled = prepared.prepares_handled;
        pending.queued_at = std::chrono::steady_clock::now();
        model_.RequestRuntimePrepare(id);
        ++it;
        continue;
      }
      if (prepared.preparing_app == id) { pending.seen_preparing = true; ++it; continue; }
      if (ready(record_of(id))) {
        plays.push_back(id);
        it = play_after_prepare_.erase(it);
        ++play_notes_revision_;
        continue;
      }
      // Prepare ended without a Ready title, or was refused before it was seen starting:
      // Play says why, so a press never ends with nothing on screen.
      // Another game's preparation finishing says nothing about this one while it is still queued.
      const bool queued = prepared.queued_prepares.count(id) > 0;
      const bool handled = !queued && prepared.prepares_handled > pending.prepares_handled && prepared.preparing_app != id;
      const bool unseen_too_long = !pending.seen_preparing && !queued && !prepared.preparing_app &&
          std::chrono::steady_clock::now() - pending.queued_at > std::chrono::seconds(15);
      if (pending.seen_preparing || handled || refusal_of(id) != pending.refusal || !game_roots_.count(id) ||
          unseen_too_long) {
        if (game_roots_.count(id) && !record_of(id)) {
          const auto refusal = refusal_of(id);
          model_.NotifyPlayStopped(id, refusal.empty() ? "The game could not be prepared." : refusal);
        } else if (const auto* record = record_of(id); record && !record->awaiting_registration) {
          model_.NotifyPlayStopped(id, !refusal_of(id).empty() ? refusal_of(id)
                                       : !record->failure.empty() ? record->failure
                                                                  : "The game could not be prepared.");
        }
        it = play_after_prepare_.erase(it);
        ++play_notes_revision_;
        continue;
      }
      ++it;
    }
  }
  for (const auto id : plays) {
    if (sync_before_play_.count(id)) continue;
    // Play checks Cloud saves first, as Steam does before a game starts; the
    // game starts once its saves are in step.
    const bool cloud_known = cloud_ && game_roots_.count(id) && !saves_root_.empty() && model_.state().cloud_saves_auto;
    if (cloud_known && !play_without_sync_.erase(id)) {
      sync_before_play_[id] = false;
      play_notes_[id] = "Checking Cloud saves before starting…";
      ++play_notes_revision_;
      continue;
    }
    play_notes_.erase(id);
    ++play_notes_revision_;
    plays_ready_.push_back(id);
  }
  std::vector<GameId> later;
  for (const auto id : plays_ready_) {
    // Never start a game while its saves are moving; it starts once they stop.
    if (cloud_busy_.count(id)) { later.push_back(id); continue; }
    play_queued_.insert(id);
    ++plays_requested_;
    runtime_->Play(id);
  }
  plays_ready_ = std::move(later);
  {
    const auto current_shared = runtime_->Shared();
    const auto& current = *current_shared;
    if (current.handoff_ready && current.launch && !cloud_busy_.count(current.launch->app_id))
      runtime_->Handoff();
  }
  const auto snapshot_shared = runtime_->Shared();
  const auto& snapshot = *snapshot_shared;
  // Queued Plays are settled once the worker has handled every request; their
  // sessions or refusals are then visible.
  if (snapshot.plays_handled >= plays_requested_) play_queued_.clear();
  if (snapshot.launch) {
    const std::pair<GameId, runtime::LaunchState> current{snapshot.launch->app_id, snapshot.launch->state};
    if (current.second == runtime::LaunchState::kExited && last_launch_ != current)
      sync_after_session_.insert(current.first);
    last_launch_ = current;
  }
  const auto announce_ready = [this] {
    for (const auto id : ready_notices_) model_.NotifyReadyToPlay(id);
    ready_notices_.clear();
  };
  if (snapshot.revision == runtime_revision_ && install_revision_ == runtime_install_revision_ &&
      cloud_revision_ == runtime_cloud_revision_ && play_notes_revision_ == runtime_notes_revision_) {
    announce_ready();
    return;
  }
  runtime_notes_revision_ = play_notes_revision_;
  runtime_revision_ = snapshot.revision;
  runtime_install_revision_ = install_revision_;
  runtime_cloud_revision_ = cloud_revision_;
  const bool usable = snapshot.status == RuntimeHostStatus::kReady && snapshot.kit_available;
  const auto state = snapshot.status == RuntimeHostStatus::kStarting ? RuntimeState::Checking
                   : snapshot.status == RuntimeHostStatus::kUnavailable ? RuntimeState::Failed
                   : usable ? RuntimeState::Ready : RuntimeState::Unavailable;
  model_.UpdateRuntimeProblem(snapshot.status == RuntimeHostStatus::kUnavailable
                                  ? (snapshot.reason.empty() ? std::string("unknown reason") : snapshot.reason)
                                  : std::string{});
  std::vector<LiveRuntimeGame> games;
  for (const auto& [id, root] : game_roots_) {
    LiveRuntimeGame game; game.game_id = id;
    const RuntimeInstallView* record = nullptr;
    for (const auto& view : snapshot.installs) if (view.app_id == id) record = &view;
    const auto refusal = snapshot.refusals.find(id);
    game.has_title = record != nullptr;
    if (snapshot.status == RuntimeHostStatus::kStarting) {
      game.status = "Checking";
    } else if (snapshot.status != RuntimeHostStatus::kReady) {
      game.status = "Unavailable"; game.detail = snapshot.reason;
    } else if (snapshot.removing_app == id) {
      game.status = "Removing"; game.detail = "Removing the game's PS5 title";
    } else if (record && record->state == runtime::PreparationState::kRemoving) {
      game.status = "Removal incomplete"; game.can_prepare = true;
      game.detail = record->failure.empty() ? "Retry finishes removing the PS5 title, then uninstalls the game."
                                            : record->failure;
    } else if (snapshot.preparing_app == id) {
      game.status = "Preparing"; game.detail = PreparationProgressWords(record != nullptr, record ? record->step : std::string{});
    } else if (record && record->state == runtime::PreparationState::kReady && !record->recovery_pending &&
               !record->title_current) {
      game.status = "Update available"; game.can_prepare = usable;
      game.detail = "Play or Update runtime updates it first; the game and its saves stay as they are";
    } else if (record && record->state == runtime::PreparationState::kReady && !record->recovery_pending &&
               record->admission != "ok") {
      game.status = "Needs repair"; game.can_prepare = usable;
      game.detail = "Play repairs the runtime first (" + record->admission + ")";
    } else if (record && record->state == runtime::PreparationState::kReady && !record->recovery_pending) {
      game.status = "Ready"; game.detail = record->title_id; game.ready = true;
    } else if (record && record->awaiting_registration) {
      // Staged and submitted: ShadowMountPlus registers it as soon as Z3CloudPlay quits,
      // so the game is ready to play from Home. Checking only asks the console.
      game.status = "Waiting for the PS5"; game.awaiting_registration = true;
      game.detail = "Waiting for the PS5, which adds the game to Home when Z3CloudPlay quits";
      game.can_prepare = usable;
    } else if (record && record->recovery_pending &&
               (record->step == "register" || record->step == "await-native" || record->step == "query-native")) {
      // The registration was submitted but not confirmed. Checking only asks the
      // console; an uncertain registration is never submitted again.
      game.status = "Registration pending";
      game.detail = "The console has not confirmed " + record->title_id +
                    " yet. Check again only asks the console; Z3CloudPlay never registers a title twice.";
      game.can_prepare = usable;
    } else if (record && record->recovery_pending) {
      game.status = "Needs recovery";
      game.detail = record->failure.empty() ? "Resume continues the saved preparation." : record->failure;
      game.can_prepare = usable;
    } else if (record && (record->state == runtime::PreparationState::kFailed ||
                          record->state == runtime::PreparationState::kUnavailable)) {
      game.status = "Failed"; game.detail = record->failure; game.can_prepare = usable;
    } else {
      game.status = "Not prepared";
      game.detail = usable ? "Play prepares it: a private bottle and the game's own PS5 title" : snapshot.kit_reason;
      game.can_prepare = usable;
    }
    if (refusal != snapshot.refusals.end() && !game.ready) game.detail = refusal->second;
    if (const auto refused = snapshot.play_refusals.find(id); refused != snapshot.play_refusals.end())
      game.play_detail = "Could not start: " + refused->second;
    const bool launching = snapshot.launch && snapshot.launch->app_id == id;
    if (launching) {
      using runtime::LaunchState;
      switch (snapshot.launch->state) {
        case LaunchState::kWaitingForCloud: game.play_status = "Waiting for Cloud"; break;
        case LaunchState::kReadyToHandoff:
        case LaunchState::kLaunching: game.play_status = "Starting…"; break;
        case LaunchState::kRunning: game.play_status = "Playing"; break;
        case LaunchState::kExited: game.play_detail = "The last session ended."; break;
        case LaunchState::kFailed:
          game.play_detail = snapshot.launch->message.empty() ? "The game could not start."
                                                              : "Could not start: " + snapshot.launch->message;
          break;
        case LaunchState::kIdle: break;
      }
    }
    // One launch at a time: Play waits while any session is undecided.
    const bool busy = snapshot.launch && (snapshot.launch->state == runtime::LaunchState::kWaitingForCloud ||
                                          snapshot.launch->state == runtime::LaunchState::kReadyToHandoff ||
                                          snapshot.launch->state == runtime::LaunchState::kLaunching ||
                                          snapshot.launch->state == runtime::LaunchState::kRunning);
    // Updating or repairing waits while this game is being started or played, and a queued
    // preparation is not asked for twice.
    if ((launching && busy) || snapshot.queued_prepares.count(id)) game.can_prepare = false;
    game.can_play = game.ready && !busy && snapshot.preparing_app != id && !cloud_busy_.count(id) &&
                    play_queued_.empty() && !sync_before_play_.count(id);
    if (const auto note = play_notes_.find(id); note != play_notes_.end() && !launching) game.play_detail = note->second;
    if (sync_before_play_.count(id)) game.play_status = "Checking saves…";
    if (const auto pending = play_after_prepare_.find(id); pending != play_after_prepare_.end()) {
      game.play_status = pending->second.words; game.can_prepare = false;
    }
    if (const auto step = uninstalls_.Step(id)) {
      game.uninstall_status = *step == UninstallStep::kSync ? "Syncing saves…"
                            : *step == UninstallStep::kRemoveTitle ? "Removing PS5 title…" : "Uninstalling…";
      game.can_play = false; game.can_prepare = false;
    }
    if (game.ready && cloud_busy_.count(id) && game.play_detail.empty()) game.play_detail = "Waiting for Cloud saves to finish.";
    if (snapshot.work_progress && (snapshot.preparing_app == id || snapshot.removing_app == id))
      game.progress = snapshot.work_progress;
    // Game settings and maintenance: the prepared title's state, the console's answer on
    // whether it runs, and the newest maintenance outcome.
    if (record) {
      game.prepared = record->state == runtime::PreparationState::kReady && !record->recovery_pending &&
                      snapshot.status == RuntimeHostStatus::kReady;
      game.settings_revision = record->settings_revision;
      game.launch_entry_id = record->launch_entry_id;
      game.save_backups = record->save_backups;
      game.latest_save_backup = record->latest_save_backup;
    }
    if (const auto answer = snapshot.activity.find(id); answer != snapshot.activity.end())
      game.activity = answer->second.first == runtime::NativeActivity::kStopped ? TitleActivity::Stopped
                    : answer->second.first == runtime::NativeActivity::kActive ? TitleActivity::Running
                                                                              : TitleActivity::Unknown;
    if (launching && busy) game.activity = TitleActivity::Running;
    if (const auto found = snapshot.maintenance.find(id); found != snapshot.maintenance.end()) {
      const auto action = found->second.action;
      game.maintenance = action == RuntimeMaintenance::kClearShaderCache ? MaintenanceKind::ClearShaderCache
                       : action == RuntimeMaintenance::kRepair ? MaintenanceKind::Repair
                       : action == RuntimeMaintenance::kBackupSaves ? MaintenanceKind::BackupSaves
                                                                    : MaintenanceKind::ApplySettings;
      game.maintenance_running = found->second.running;
      game.maintenance_ok = found->second.ok;
      game.maintenance_message = found->second.message;
    }
    if (recreate_.count(id)) {
      game.maintenance = MaintenanceKind::RecreateBottle;
      game.maintenance_running = true;
      game.maintenance_message.clear();
    } else if (const auto result = recreate_results_.find(id); result != recreate_results_.end() &&
               (!game.maintenance || !game.maintenance_running)) {
      game.maintenance = MaintenanceKind::RecreateBottle;
      game.maintenance_running = false;
      game.maintenance_ok = result->second.first;
      game.maintenance_message = result->second.second;
    }
    games.push_back(std::move(game));
  }
  model_.SetGeneratedRecipes(snapshot.generated_recipes);
  {
    std::map<GameId, LiveGameSetup> setups;
    for (const auto& [app, setup] : snapshot.game_setups) {
      auto& live = setups[app];
      live.programs = setup.programs;
      live.scanned = setup.scanned;
      live.draft = setup.draft;
      live.refusal_soft = setup.refusal_soft;
      if (const auto refusal = snapshot.refusals.find(app); refusal != snapshot.refusals.end()) live.refusal = refusal->second;
    }
    model_.SetGameSetups(std::move(setups));
  }
  // Game settings rows follow what the verified kit can apply; no kit applies nothing gated.
  if (snapshot.status != RuntimeHostStatus::kStarting) {
    auto capabilities = snapshot.kit ? snapshot.kit->capabilities : std::vector<std::string>{};
    if (kit_capabilities_ != capabilities) {
      model_.SetKitCapabilities(runtime::KitCapabilitiesFromManifest(capabilities, true));
      kit_capabilities_ = std::move(capabilities);
    }
  }
  model_.UpdateRuntime(state, games);
  model_.SetRuntimeLeftovers(snapshot.leftover_requests);
  LiveKitInfo kit;
  if (snapshot.kit)
    kit = {snapshot.kit->view.kit_id, snapshot.kit->manifest_sha256, snapshot.kit->expected_manifest_sha256,
           snapshot.kit->prospero_win_revision, snapshot.kit->source_dirty, {}};
  else
    kit.unavailable_reason = snapshot.status == RuntimeHostStatus::kReady ? snapshot.kit_reason : std::string{};
  model_.SetKitInfo(std::move(kit));
  announce_ready();
}
void Controller::BeginUninstall(GameId id, bool delete_saves) {
  // Without the runtime there is no PS5 title to remove: the download goes as before.
  if (!runtime_) { if (installs_) installs_->Uninstall(id); return; }
  // With automatic sync off, nothing moves to or from Steam Cloud unless the player syncs from Saves.
  if (uninstalls_.Begin(id, delete_saves, cloud_ && game_roots_.count(id) && !saves_root_.empty() && model_.state().cloud_saves_auto))
    ++play_notes_revision_;
}
void Controller::StopUninstall(GameId id, const std::string& reason) {
  ++play_notes_revision_;
  model_.NotifyUninstallStopped(id, reason);
}
void Controller::TickUninstalls() {
  if (!runtime_) return;
  const auto ids = uninstalls_.Active();
  if (ids.empty()) return;
  const auto snapshot_shared = runtime_->Shared();
  const auto& snapshot = *snapshot_shared;
  if (snapshot.status == RuntimeHostStatus::kStarting) return;
  for (const auto id : ids) {
    UninstallFacts facts;
    facts.has_title = std::any_of(snapshot.installs.begin(), snapshot.installs.end(),
                                  [id](const RuntimeInstallView& view) { return view.app_id == id; });
    facts.removing = snapshot.removing_app == id;
    facts.removals_handled = snapshot.removals_handled;
    if (const auto failure = snapshot.removal_failures.find(id); failure != snapshot.removal_failures.end())
      facts.removal_failure = failure->second;
    facts.saves_moving = cloud_busy_.count(id) != 0;
    facts.session_active = SessionActive(id);
    const auto action = uninstalls_.Advance(id, facts);
    switch (action.kind) {
      case UninstallAction::Kind::kNone: break;
      case UninstallAction::Kind::kRemoveTitle: runtime_->Remove(id, action.delete_saves); ++play_notes_revision_; break;
      // The title is gone, so no title points at the files about to be deleted.
      case UninstallAction::Kind::kRemoveDownload: if (installs_) installs_->Uninstall(id); ++play_notes_revision_; break;
      case UninstallAction::Kind::kStop: StopUninstall(id, action.reason); break;
    }
  }
}
void Controller::TickInstalls() {
  if (!installs_) return;
  for (const auto& request : model_.TakeInstallRequests()) {
    switch (request.kind) {
      case InstallRequestKind::Install: {
        const auto& games = model_.state().games;
        const auto game = std::find_if(games.begin(), games.end(), [&](const Game& value) {
          return value.id == request.game_id;
        });
        installs_->Install(request.game_id, game == games.end() ? std::nullopt : game->install_bytes, request.volume_id);
        break;
      }
      case InstallRequestKind::Pause: installs_->Pause(request.game_id); break;
      case InstallRequestKind::Resume: installs_->Resume(request.game_id); break;
      case InstallRequestKind::DownloadNow: installs_->DownloadNow(request.game_id); break;
      case InstallRequestKind::MoveUp: installs_->MoveQueued(request.game_id, true); break;
      case InstallRequestKind::MoveDown: installs_->MoveQueued(request.game_id, false); break;
      case InstallRequestKind::Cancel: installs_->Cancel(request.game_id); break;
      case InstallRequestKind::Uninstall: BeginUninstall(request.game_id, request.delete_saves); break;
      case InstallRequestKind::SetDefaultVolume: installs_->SetDefaultVolume(request.volume_id); break;
      case InstallRequestKind::DismissCompleted: installs_->DismissCompleted(request.game_id); break;
      case InstallRequestKind::ClearCompleted: installs_->ClearCompleted(); break;
    }
  }
  const auto snapshot = installs_->Snapshot();
  game_roots_.clear();
  game_volumes_.clear();
  for (const auto& game : snapshot.installed)
    if (game.available) { game_roots_[game.id] = game.root; game_volumes_[game.id] = game.volume_id; }
  // A download that finished in this session is prepared next, so the game is ready to play.
  std::set<GameId> downloading;
  for (const auto& job : snapshot.jobs) downloading.insert(job.id);
  for (const auto id : downloading_)
    if (!downloading.count(id) && game_roots_.count(id)) auto_prepare_.insert(id);
  downloading_ = std::move(downloading);
  if (!snapshot.volumes.empty()) saves_root_ = snapshot.volumes.front().games_root + "/.z3cloudplay-saves";
  // Free space changes outside Z3CloudPlay too, so volumes refresh every few seconds as well.
  const auto now = std::chrono::steady_clock::now();
  const bool account_changed = model_.state().account_generation != install_generation_;
  const bool space_due = now - volume_refreshed_at_ >= std::chrono::seconds(10);
  if (snapshot.revision == install_revision_ && !account_changed && !space_due) return;
  {
    std::vector<LiveVolume> volumes;
    for (const auto& volume : snapshot.volumes) {
      LiveVolume live{volume.id, volume.label, volume.available, std::nullopt, std::nullopt};
      if (volume.available) {
        const auto space = steam::QueryVolumeSpace(volume.games_root);
        if (space) { live.total_bytes = space->total_bytes; live.free_bytes = space->free_bytes; }
      }
      volumes.push_back(std::move(live));
    }
    model_.SetFreeSpaceSupported(steam::VolumeSpaceSupported());
    model_.UpdateVolumes(volumes, snapshot.default_volume_id);
    volume_refreshed_at_ = now;
  }
  if (snapshot.revision == install_revision_ && !account_changed) return;
  install_revision_ = snapshot.revision;
  install_generation_ = model_.state().account_generation;
  std::vector<LiveInstallJob> jobs;
  for (const auto& job : snapshot.jobs) {
    JobState state = JobState::Queued;
    switch (job.state) {
      case steam::InstallJobState::kQueued: state = JobState::Queued; break;
      case steam::InstallJobState::kRunning: state = JobState::Downloading; break;
      case steam::InstallJobState::kPaused: state = JobState::Paused; break;
      case steam::InstallJobState::kFailed: state = JobState::Failed; break;
    }
    jobs.push_back({job.id, state, job.done_bytes, job.total_bytes, job.failure, job.volume_id,
                    job.bytes_per_second, job.eta_seconds, job.held_bytes});
  }
  std::vector<LiveInstalledGame> installed;
  for (const auto& game : snapshot.installed) installed.push_back({game.id, game.install_bytes, game.volume_id, game.available});
  std::vector<LiveCompletedDownload> completed;
  for (const auto& finished : snapshot.completed)
    completed.push_back({finished.id, finished.install_bytes, finished.volume_id, finished.finished_at});
  model_.UpdateInstalls(jobs, installed, completed);
}
void Controller::TickStoreDetails() {
  if (!store_details_) return;
  // The store is public: a game's description is asked for when its details open.
  const auto& detail = model_.state().navigation.detail_id;
  if (detail && detail != requested_store_detail_) {
    requested_store_detail_ = detail;
    store_details_->Request(*detail);
  }
  for (auto& details : store_details_->Pump())
    model_.UpdateStoreDetails(details.id, std::move(details.description), std::move(details.genres),
                              std::move(details.controller_support));
}
void Controller::TickAppDetails() {
  if (!app_details_) return;
  const auto& state = model_.state();
  const auto generation = state.account_generation;
  if (generation != requested_generation_) {
    app_details_->Invalidate(generation);
    requested_generation_ = generation;
    requested_detail_.reset();
    launch_options_.clear();
    launch_options_reason_.clear();
    prepare_after_details_.clear();
  }
  // Fetch Steam app info when the player opens a game's details while signed in.
  if (state.account.authentication == AuthState::SignedIn && state.navigation.detail_id &&
      state.navigation.detail_id != requested_detail_) {
    requested_detail_ = state.navigation.detail_id;
    app_details_->Request(*state.navigation.detail_id, generation);
  }
  for (const auto& details : app_details_->Pump()) {
    if (details.account_generation != generation) continue;
    if (details.available) {
      model_.UpdateAppDetails(details.id, details.download_bytes, details.install_bytes, details.windows_installable);
      launch_options_[details.id] = details.launches;
      launch_options_reason_.erase(details.id);
    } else if (!launch_options_.count(details.id)) {
      launch_options_reason_[details.id] = details.reason.empty() ? "Steam details are unavailable right now." : details.reason;
    }
    if (prepare_after_details_.erase(details.id) && runtime_) SubmitPrepare(details.id);
  }
}
void Controller::SubmitPrepare(GameId id, bool announce) {
  const auto root = game_roots_.find(id);
  if (!runtime_ || root == game_roots_.end()) return;
  if (announce) ready_watch_[id] = {false, std::chrono::steady_clock::now()};
  else ready_watch_.erase(id);
  RuntimePrepareRequest request{id, root->second, game_volumes_[id], {}};
  for (const auto& game : model_.state().games) if (game.id == id) request.title_name = game.title;
  if (model_.state().title_store_suffix && !request.title_name.empty()) request.title_name += " (Steam)";
  if (const auto found = launch_options_.find(id); found != launch_options_.end())
    for (const auto& option : found->second)
      request.launches.push_back({"", option.description, option.executable, option.working_directory, option.arguments});
  if (const auto reason = launch_options_reason_.find(id); reason != launch_options_reason_.end())
    request.launch_reason = "Z3CloudPlay could not read how to start this game from Steam: " + reason->second;
  else if (!launch_options_.count(id))
    request.launch_reason = "Sign in to Steam so Z3CloudPlay can read how to start this game.";
  // After a failed answer, ask Steam again so a later Prepare can use the options.
  if (app_details_ && !launch_options_.count(id)) app_details_->Request(id, model_.state().account_generation);
  runtime_->Prepare(std::move(request));
}
void Controller::Apply(const steam::SteamEvent& event) {
  if (event.generation < generation_) return;
  if (phase_ == Phase::SignOut) {
    if (event.kind != steam::SteamEventKind::kSignedOut && event.kind != steam::SteamEventKind::kFailure) return;
  } else if (phase_ == Phase::Idle || phase_==Phase::ClearLibrary || phase_==Phase::ClearOrganization || event.request_id != request_) return;
  generation_ = event.generation;
  switch (event.kind) {
    case steam::SteamEventKind::kChallenge:
      if (phase_ == Phase::SignIn) Update(AuthState::Waiting,{},false,event.challenge_url);
      break;
    case steam::SteamEventKind::kAwaitingApproval:
      break;  // The preceding challenge remains on screen; do not erase it.
    case steam::SteamEventKind::kOfflineSession:
    case steam::SteamEventKind::kAuthenticated: {
      phase_ = Phase::Idle;
      retry_saved_session_ = false;
      reauthenticating_ = false;
      const bool offline=event.kind==steam::SteamEventKind::kOfflineSession;
      established_auth_=offline?AuthState::Offline:AuthState::SignedIn;
      AccountUpdate update;
      update.authentication = established_auth_;
      update.id = std::to_string(event.steam_id); update.display_name = event.account_name;
      update.clear_library = model_.state().account.id!=update.id;
      if(!library_) update.error = "Library sync is unavailable. Try again later.";
      model_.UpdateAccount(std::move(update));
      if(persistence_) persistence_->Activate(event.steam_id);
      if(runtime_) runtime_->SetSteamAccount(event.account_name, event.steam_id);
      if(library_) library_->Activate(event.steam_id,offline);
      break;
    }
    case steam::SteamEventKind::kSignedOut: {
      phase_ = Phase::Idle;
      established_auth_=AuthState::SignedOut;
      if(library_) library_->Reset();
      if(persistence_) persistence_->Reset();
      if(runtime_) runtime_->SetSteamAccount({}, 0);
      const bool begin = begin_after_clear_; begin_after_clear_ = false;
      Update(AuthState::SignedOut,{},true);
      if (begin) Begin();
      break;
    }
    case steam::SteamEventKind::kCanceled:
      phase_ = Phase::Idle;
      if (reauthenticating_) { reauthenticating_ = false; Restore(); break; }
      Update(AuthState::Cancelled,{},!retry_saved_session_); break;
    case steam::SteamEventKind::kFailure: {
      if (reauthenticating_ && phase_ == Phase::SignIn) {
        // The new QR sign-in failed or expired: keep using the saved one.
        reauthenticating_ = false;
        phase_ = Phase::Idle;
        Restore();
        break;
      }
      const bool restoring = phase_ == Phase::Restore;
      const bool clearing = phase_ == Phase::SignOut;
      phase_ = Phase::Idle; begin_after_clear_ = false;
      if (restoring && event.error == steam::AuthError::kPersistence) {
        // A busy/private storage probe is not a revoked Steam credential. Keep
        // account state, retry the saved session, and never start QR sign-in.
        retry_saved_session_ = true;
        retry_saved_session_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        Update(AuthState::Failed, "Account storage is unavailable. Retrying your saved sign-in…");
        break;
      }
      retry_saved_session_ = false;
      if(!clearing) { established_auth_=AuthState::SignedOut;if(library_)library_->Reset();if(persistence_)persistence_->Reset(); }
      if (clearing) Update(model_.state().account.id.empty() ? AuthState::Failed : established_auth_,
                           "Sign-out failed. Your saved account remains. Retry sign-out in Settings.");
      else if (restoring && event.error == steam::AuthError::kNoSession) Update(AuthState::SignedOut,{},true);
      else if (restoring && (event.error == steam::AuthError::kRejected || event.error == steam::AuthError::kCredentialsRevoked)) {
        // Steam no longer accepts the saved session: show a new QR code right away
        // instead of a failure the player can only retry.
        Update(AuthState::SignedOut,{},true);
        Begin();
      }
      else if (event.error == steam::AuthError::kCanceled) Update(AuthState::Cancelled,{},true);
      else if (event.error == steam::AuthError::kExpired) Update(AuthState::Expired,diagnostics::AuthMessage(event.error),true);
      else Update(AuthState::Failed,diagnostics::AuthMessage(event.error),true);
      break;
    }
  }
}
}
