#include "steam/service.h"
#include <algorithm>

namespace z3cloudplay::steam {
namespace {
constexpr std::size_t kMaxEvents = 64;
SteamEvent Failure(AuthError error) {
  SteamEvent event;
  event.kind = SteamEventKind::kFailure;
  event.error = error;
  return event;
}
bool StructurallyValid(const StoredSession& session) {
  return session.steam_id != 0 && !session.refresh_token.empty();
}
}

SteamService::SteamService(AuthBackend& backend, SessionRepository& repository)
    : backend_(backend), repository_(repository), worker_([this] { Run(); }) {}

SteamService::~SteamService() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    if (cancel_) cancel_->store(true);
    // A requested sign-out must still clear storage during orderly shutdown.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
        [](const Job& job) { return job.operation != Operation::kClear; }), pending_.end());
  }
  wake_.notify_all();
  worker_.join();
}

RequestId SteamService::Submit(Operation operation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (cancel_) cancel_->store(true);
  cancel_ = std::make_shared<std::atomic<bool>>(false);
  active_ = true;
  ++request_;
  ++generation_;
  events_.clear();
  if (operation == Operation::kClear) {
    restore_blocked_ = true;
    pending_.clear();
  } else {
    // Retain a sign-out barrier even when another command replaces pending work.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
        [](const Job& job) { return job.operation != Operation::kClear; }), pending_.end());
  }
  pending_.push_back({operation, request_, generation_, cancel_});
  wake_.notify_one();
  return request_;
}

RequestId SteamService::BeginQrSignIn() { return Submit(Operation::kBegin); }
RequestId SteamService::RestoreSession() { return Submit(Operation::kRestore); }
void SteamService::SignOut() { Submit(Operation::kClear); }

void SteamService::Cancel(RequestId request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (request != request_ || !cancel_ || !active_) return;
  active_ = false;
  cancel_->store(true);
  ++generation_;
  events_.clear();
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
      [](const Job& job) { return job.operation != Operation::kClear; }), pending_.end());
  SteamEvent event;
  event.kind = SteamEventKind::kCanceled;
  event.request_id = request;
  event.generation = generation_;
  event.error = AuthError::kCanceled;
  events_.push_back(std::move(event));
}

bool SteamService::Current(const Job& job) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && job.generation == generation_ && !job.cancel->load();
}

void SteamService::Emit(const Job& job, SteamEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || job.generation != generation_ || job.cancel->load()) return;
  if (event.kind == SteamEventKind::kAuthenticated || event.kind == SteamEventKind::kOfflineSession ||
      event.kind == SteamEventKind::kSignedOut || event.kind == SteamEventKind::kFailure ||
      event.kind == SteamEventKind::kCanceled) active_ = false;
  event.request_id = job.request;
  event.generation = job.generation;
  if (events_.size() == kMaxEvents) events_.pop_front();
  events_.push_back(std::move(event));
}

std::vector<SteamEvent> SteamService::Pump(std::size_t max_events) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SteamEvent> result;
  const auto count = std::min({max_events, events_.size(), kMaxEvents});
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return result;
}

void SteamService::Run() {
  while (true) {
    std::optional<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (pending_.empty() && stopping_) return;
      job = pending_.front();
      pending_.pop_front();
    }
    try { Execute(*job); }
    catch (...) { Emit(*job, Failure(AuthError::kProtocol)); }
  }
}

void SteamService::Execute(const Job& job) {
  if (job.operation == Operation::kClear) {
    const auto cleared = repository_.Clear();
    if (!cleared.ok()) { Emit(job, Failure(AuthError::kPersistence)); return; }
    revocation_cleanup_pending_ = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      restore_blocked_ = false;
    }
    SteamEvent event;
    event.kind = SteamEventKind::kSignedOut;
    Emit(job, std::move(event));
    return;
  }
  if (!Current(job)) return;
  AuthResult result;
  std::uint64_t restore_identity = 0;
  std::string restore_account_name;
  if (job.operation == Operation::kRestore) {
    if (revocation_cleanup_pending_) {
      // Never submit a known-revoked token again. Storage may have recovered
      // since the first deletion attempt; complete it before requesting QR.
      if (!repository_.Clear().ok()) {
        Emit(job, Failure(AuthError::kPersistence));
        return;
      }
      revocation_cleanup_pending_ = false;
      { std::lock_guard<std::mutex> lock(mutex_); restore_blocked_ = false; }
      Emit(job, Failure(AuthError::kCredentialsRevoked));
      return;
    }
    bool blocked;
    { std::lock_guard<std::mutex> lock(mutex_); blocked = restore_blocked_; }
    if (blocked) { Emit(job, Failure(AuthError::kPersistence)); return; }
    auto loaded = repository_.Load();
    if (!Current(job)) return;
    if (!loaded.ok()) { Emit(job, Failure(AuthError::kPersistence)); return; }
    if (!loaded.value()) { Emit(job, Failure(AuthError::kNoSession)); return; }
    if (!StructurallyValid(*loaded.value())) { Emit(job, Failure(AuthError::kProtocol)); return; }
    restore_identity = loaded.value()->steam_id;
    restore_account_name = loaded.value()->account_name;
    result = backend_.Restore(*loaded.value(), *job.cancel);
  } else {
    result = backend_.AuthenticateQr(*job.cancel, [this, &job](const std::string& url) {
      if (url.empty() || url.size() > 4096) return;
      SteamEvent event;
      event.kind = SteamEventKind::kChallenge;
      event.challenge_url = url;
      Emit(job, std::move(event));
      event = {};
      event.kind = SteamEventKind::kAwaitingApproval;
      Emit(job, std::move(event));
    });
  }
  if (!Current(job)) return;
  if (result.error != AuthError::kNone) {
    if (job.operation == Operation::kRestore && result.error == AuthError::kNetwork) {
      SteamEvent event;
      event.kind = SteamEventKind::kOfflineSession;
      event.error = AuthError::kNetwork;
      event.steam_id = restore_identity;
      event.account_name = std::move(restore_account_name);
      Emit(job, std::move(event));
      return;
    }
    if (result.error == AuthError::kCredentialsRevoked) {
      { std::lock_guard<std::mutex> lock(mutex_); restore_blocked_ = true; }
      if (!repository_.Clear().ok()) {
        revocation_cleanup_pending_ = true;
        Emit(job, Failure(AuthError::kPersistence));
        return;
      }
      { std::lock_guard<std::mutex> lock(mutex_); restore_blocked_ = false; }
    }
    Emit(job, Failure(result.error));
    return;
  }
  if (!result.session || !result.session->steam_id || result.session->refresh_token.empty() ||
      (restore_identity && result.session->steam_id != restore_identity)) {
    Emit(job, Failure(AuthError::kProtocol));
    return;
  }
  // I/O runs without the command mutex. A cancellation/replacement during this
  // write cannot publish the session and must remove that stale write before
  // the single worker executes any newer command.
  const auto saved = repository_.Save(*result.session);
  bool stale;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stale = job.generation != generation_;
    if (!stale && saved.ok()) restore_blocked_ = false;
  }
  if (stale) {
    const auto cleared = repository_.Clear();
    if (!cleared.ok()) { std::lock_guard<std::mutex> lock(mutex_); restore_blocked_ = true; }
    return;
  }
  if (!saved.ok()) { Emit(job, Failure(AuthError::kPersistence)); return; }
  revocation_cleanup_pending_ = false;
  SteamEvent event;
  event.kind = SteamEventKind::kAuthenticated;
  event.steam_id = result.session->steam_id;
  event.account_name = result.session->account_name;
  Emit(job, std::move(event));
}

}  // namespace z3cloudplay::steam
