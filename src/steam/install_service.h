#pragma once

#include "library/model.h"
#include "steam/session_store.h"
#include "steam/download_rate.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace z3cloudplay::steam {

struct InstallRequest {
  enum class Action { kInstall, kUninstall, kCancel };
  Action action = Action::kInstall;
  std::uint32_t app = 0;
};
// Development request file: strict app IDs, preserving command order, at most 64 commands.
std::vector<InstallRequest> ParseInstallRequests(const std::string& text);

enum class InstallJobState { kQueued, kRunning, kPaused, kFailed };

// A place games can be installed: a folder on one drive. `available` means the folder exists and
// Z3CloudPlay can write to it now; installs recorded on a volume that is absent stay listed.
struct InstallVolume {
  std::string id;          // stable across runs, e.g. "internal"
  std::string label;       // shown to the player
  std::string games_root;  // absolute folder holding <app id> folders
  bool available = true;
};

struct InstallJobView {
  GameId id = 0;
  InstallJobState state = InstallJobState::kQueued;
  std::uint64_t done_bytes = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t held_bytes = 0;  // installed bytes already on disk (see PendingInstallBytes)
  std::string failure;  // short, user-facing
  std::string volume_id;
  std::optional<std::uint64_t> bytes_per_second;
  std::optional<int> eta_seconds;
};

struct InstalledGame {
  GameId id = 0;
  std::uint64_t install_bytes = 0;
  std::string root;
  std::string volume_id;
  bool available = true;  // false while the game's volume is absent
  std::int64_t finished_at = 0;  // Unix seconds the install finished (its record's time); 0 unknown
};

// A recent finished download that Downloads lists after the queue until the player dismisses it.
struct CompletedDownload {
  GameId id = 0;
  std::uint64_t install_bytes = 0;
  std::string volume_id;
  std::int64_t finished_at = 0;
};

// Installs that finished at most `kCompletedDays` before `now` and were not dismissed (a dismissal
// names the finish time, so installing the game again lists it again), newest first, at most
// `kMaxCompleted`.
inline constexpr std::int64_t kCompletedDays = 7;
inline constexpr std::size_t kMaxCompleted = 20;
std::vector<CompletedDownload> CompletedDownloads(const std::vector<InstalledGame>& installed,
                                                  const std::vector<std::pair<GameId, std::int64_t>>& dismissed,
                                                  std::int64_t now);

struct InstallSnapshot {
  std::uint64_t revision = 0;
  std::vector<InstallJobView> jobs;
  std::vector<InstalledGame> installed;
  std::vector<CompletedDownload> completed;
  std::vector<InstallVolume> volumes;
  std::string default_volume_id;
};

// One game's install, run synchronously on the service worker. Implementations report progress
// and honor cancel; they return an empty string on success or a short failure text.
class InstallRunner {
 public:
  virtual ~InstallRunner() = default;
  struct Result {
    bool ok = false;
    bool canceled = false;
    std::string failure;
    std::uint64_t install_bytes = 0;
    bool retry_later = false;  // Steam or the network was out of reach: the job waits and tries again
  };
  virtual Result Install(const StoredSession& session, GameId id, const std::string& root,
                         const std::atomic<bool>& cancel,
                         const std::function<void(std::uint64_t done, std::uint64_t total, std::uint64_t held)>& progress) = 0;
  virtual bool Remove(const std::string& root) = 0;
};

// Install queue for real games under <volume games_root>/<app id>. One install runs at a time.
// Pause keeps the partial download for Resume; Cancel and Uninstall delete only Z3CloudPlay-managed
// folders. The first volume is primary: it holds the registry of every managed game (with the
// volume each is on) and the default-volume choice. At start the registry is read: complete
// installs are listed and interrupted downloads come back paused. New installs go to the default
// volume. The runner and repository outlive the service.
class InstallService {
 public:
  // One always-available volume, id "internal".
  InstallService(InstallRunner& runner, SessionRepository& sessions, std::string games_root);
  // `volumes` must be non-empty; volumes[0] is primary and must be available.
  InstallService(InstallRunner& runner, SessionRepository& sessions, std::vector<InstallVolume> volumes);
  ~InstallService();
  InstallService(const InstallService&) = delete;
  InstallService& operator=(const InstallService&) = delete;

  // Installs to `volume_id`, or the default drive when empty; the default is not changed. The
  // estimate is reserved (less what the download already holds on disk) until the job ends, and
  // the runner still checks the selected manifests before writing game data.
  void Install(GameId id, std::optional<std::uint64_t> estimated_bytes = std::nullopt,
               const std::string& volume_id = {});
  void Pause(GameId id);
  void Resume(GameId id);
  // Moves the download to the front of the queue and starts it. A download that was running stops
  // and stays queued right behind it, so it carries on by itself afterwards.
  void DownloadNow(GameId id);
  // Swaps with the previous/next queued download, preserving the running download.
  void MoveQueued(GameId id, bool up);
  void Cancel(GameId id);
  void Uninstall(GameId id);
  // Chooses the volume for installs queued from now on; ignored unless it names an available volume.
  void SetDefaultVolume(const std::string& volume_id);
  // Takes a finished download, or every one, off the Completed list in Downloads.
  void DismissCompleted(GameId id);
  void ClearCompleted();
  InstallSnapshot Snapshot();
  void Shutdown();

  // The folder a game uses (or would use if installed now); empty if its volume is unknown.
  std::string RootFor(GameId id);
  static constexpr std::size_t kMaxJobs = 64;
  static constexpr const char* kPrimaryVolumeId = "internal";

 private:
  struct Job {
    InstallJobView view;
    DownloadRate rate;
    bool cancel_requested = false;  // delete files when the run stops
    std::optional<std::uint64_t> estimated_bytes;
  };
  void Run();
  void Scan();
  Job* Find(GameId id);
  void Publish();
  void SaveRegistry();
  void SaveDefaultVolume();
  void SaveDismissed();
  void Dismiss(GameId id);
  const InstallVolume* FindVolume(const std::string& id) const;
  std::string RootOn(const std::string& volume_id, GameId id) const;
  std::string LockedRootFor(GameId id) const;

  InstallRunner& runner_;
  SessionRepository& sessions_;
  std::vector<InstallVolume> volumes_;
  std::string default_volume_id_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::vector<InstalledGame> installed_;
  std::vector<std::pair<GameId, std::int64_t>> dismissed_;  // finished downloads taken off Completed
  std::deque<std::string> removals_;  // folders to delete
  std::optional<GameId> active_;
  std::atomic<bool> active_cancel_{false};
  std::uint64_t revision_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace z3cloudplay::steam
