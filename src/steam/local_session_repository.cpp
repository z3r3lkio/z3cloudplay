#include "steam/local_session_repository.h"
#include <cerrno>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>

namespace z3cloudplay {
LocalSessionRepository::LocalSessionRepository(std::string root, DiagnosticReport report)
    : root_(std::move(root)),storage_(root_),store_(storage_),report_(std::move(report)) {}
Result<void> LocalSessionRepository::Prepare() const {
  if (prepared_) return Result<void>::Success();
  if (root_.empty() || root_.find('\0') != std::string::npos)
    return Result<void>::Failure(StorageError::kInvalidPath,"Session storage path is invalid");
  if (mkdir(root_.c_str(),0700) != 0 && errno != EEXIST) {
    const int error = errno;
    if (report_) report_("session storage mkdir failed errno=" + std::to_string(error));
    return Result<void>::Failure(StorageError::kOpenFailed,"Session storage is unavailable");
  }
  struct stat info{};
  const int stat_result = stat(root_.c_str(),&info);
  const int stat_error = stat_result ? errno : 0;
  if (stat_result != 0 || !S_ISDIR(info.st_mode)) {
    if (report_) report_("session storage stat failed errno=" + std::to_string(stat_error) +
                        " directory=" + std::to_string(S_ISDIR(info.st_mode)));
    return Result<void>::Failure(StorageError::kOpenFailed,"Session storage is unavailable");
  }
  const Bytes marker{'o','k'};
  // Each service owns a repository for this root. A shared probe (and .tmp)
  // lets one worker rename/remove another worker's file during startup.
  static std::atomic<unsigned long long> next_probe{0};
  const std::string path = "session-storage.probe-" + std::to_string(getpid()) +
                           "-" + std::to_string(next_probe.fetch_add(1, std::memory_order_relaxed));
  const auto written = storage_.WriteAtomicPrivate(path,marker);
  if (!written.ok()) {
    if (report_) report_("session storage private probe failed: " + written.message());
    return Result<void>::Failure(written.error(),"Private session storage is unavailable");
  }
  const auto loaded = storage_.Read(path,marker.size());
  const auto removed = storage_.Remove(path);
  if (!loaded.ok() || loaded.value() != marker || !removed.ok()) {
    if (report_) {
      if (!loaded.ok()) report_("session storage probe read failed: " + loaded.message());
      else if (loaded.value() != marker) report_("session storage probe contents changed: " + path);
      if (!removed.ok()) report_("session storage probe remove failed: " + removed.message());
    }
    return Result<void>::Failure(StorageError::kReadFailed,"Session storage validation failed");
  }
  prepared_ = true;
  return Result<void>::Success();
}
Result<std::optional<StoredSession>> LocalSessionRepository::Load() const {
  const auto ready = Prepare();
  if (!ready.ok()) return Result<std::optional<StoredSession>>::Failure(ready.error(),ready.message());
  auto result = store_.Load();
  if (!result.ok() && report_) report_("session storage load failed: " + result.message());
  return result;
}
Result<void> LocalSessionRepository::Save(const StoredSession& session) const {
  const auto ready = Prepare();
  if (!ready.ok()) return ready;
  return store_.Save(session);
}
Result<void> LocalSessionRepository::Clear() const {
  // Clearing must remain possible even if the private-write probe now fails.
  if (root_.empty() || root_.find('\0') != std::string::npos)
    return Result<void>::Failure(StorageError::kInvalidPath,"Session storage path is invalid");
  return store_.Clear();
}
}
