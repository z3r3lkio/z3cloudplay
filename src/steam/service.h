#pragma once

#include "steam/auth_backend.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace z3cloudplay::steam {

using RequestId = std::uint64_t;
enum class SteamEventKind {
  kChallenge,
  kAwaitingApproval,
  kAuthenticated,
  kOfflineSession,
  kSignedOut,
  kCanceled,
  kFailure
};
struct SteamEvent {
  SteamEventKind kind = SteamEventKind::kFailure;
  RequestId request_id = 0;
  std::uint64_t generation = 0;
  AuthError error = AuthError::kNone;
  std::string challenge_url;
  std::uint64_t steam_id = 0;
  std::string account_name;
};

// Commands enqueue work and invalidate older results without doing storage or
// network I/O on the caller. Backend and repository must outlive this service.
// SignedOut is emitted only after Clear succeeds; failure is explicit.
class SteamService {
 public:
  SteamService(AuthBackend& backend, SessionRepository& repository);
  ~SteamService();
  SteamService(const SteamService&) = delete;
  SteamService& operator=(const SteamService&) = delete;
  RequestId BeginQrSignIn();
  RequestId RestoreSession();
  void Cancel(RequestId request);
  void SignOut();
  std::vector<SteamEvent> Pump(std::size_t max_events);

 private:
  enum class Operation { kBegin, kRestore, kClear };
  struct Job {
    Operation operation;
    RequestId request;
    std::uint64_t generation;
    std::shared_ptr<std::atomic<bool>> cancel;
  };
  RequestId Submit(Operation operation);
  bool Current(const Job& job) const;
  void Emit(const Job& job, SteamEvent event);
  void Run();
  void Execute(const Job& job);
  AuthBackend& backend_;
  SessionRepository& repository_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> pending_;
  std::shared_ptr<std::atomic<bool>> cancel_;
  std::deque<SteamEvent> events_;
  RequestId request_ = 0;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  bool active_ = false;
  bool restore_blocked_ = false;
  // Worker-owned: retry deletion of credentials already rejected by Steam.
  bool revocation_cleanup_pending_ = false;
  std::thread worker_;
};

}  // namespace z3cloudplay::steam
