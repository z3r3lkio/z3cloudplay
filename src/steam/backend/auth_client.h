#pragma once

#include "steam/auth_backend.h"
#include "steam/backend/http_client.h"
#include <functional>
#include <memory>

namespace z3cloudplay::steam {

// Deterministic protocol tests inject transport and clocks. Production uses
// verified HttpClient, steady_clock and cancel-aware waits. These callbacks must
// be bounded and run only on the owning service worker.
struct AuthClientHooks {
  std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)> perform;
  std::function<std::uint64_t()> monotonic_ms;
  std::function<std::uint64_t()> unix_seconds;
  std::function<bool(std::uint32_t, const std::atomic<bool>&)> wait;
  // Optional sanitized failure report: method name, HTTP status and X-EResult number only.
  std::function<void(const char* method, long http_status, int eresult)> failure;
  // Optional renewal through an authenticated CM session, used when the Web API refuses a
  // SteamClient refresh token with AccessDenied. Returns the updated session or an error.
  std::function<AuthResult(const StoredSession&, const std::atomic<bool>&)> cm_renew;
  // Optional report of a successful restore: which path (stored, web, cm), and the refresh token's
  // fingerprint before and after (they differ when Steam rotated it).
  std::function<void(const char* path, const std::string& before, const std::string& after)> restored;
};

class AuthClient final : public AuthBackend {
 public:
  // No network initialization occurs until a service-worker call.
  explicit AuthClient(std::string ca_bundle);
  explicit AuthClient(AuthClientHooks hooks);
  // Installs the sanitized failure report on a production client before first use.
  void SetFailureReport(std::function<void(const char*, long, int)> report) {
    hooks_.failure = std::move(report);
  }
  void SetRestoreReport(std::function<void(const char*, const std::string&, const std::string&)> report) {
    hooks_.restored = std::move(report);
  }
  void SetCmRenewal(std::function<AuthResult(const StoredSession&, const std::atomic<bool>&)> renew) {
    hooks_.cm_renew = std::move(renew);
  }
  AuthResult AuthenticateQr(const std::atomic<bool>& cancel,
                            const ChallengeCallback& challenge) override;
  AuthResult Restore(const StoredSession& session,
                     const std::atomic<bool>& cancel) override;
 private:
  AuthError Post(const char* method, const std::vector<std::uint8_t>& message,
                 const std::atomic<bool>& cancel, std::uint32_t timeout_ms,
                 bool restoring, std::vector<std::uint8_t>* body);
  AuthResult Renew(const StoredSession& session, const std::atomic<bool>& cancel,
                   std::uint32_t timeout_ms, bool restoring);
  AuthResult Restored(const char* path, const StoredSession& before, AuthResult result);
  AuthClientHooks hooks_;
  int last_eresult_ = 1;  // X-EResult of the most recent refused Post, for renewal fallback
  std::string ca_bundle_;
  std::unique_ptr<HttpClient> http_;
};

}  // namespace z3cloudplay::steam
