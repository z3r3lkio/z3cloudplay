#pragma once

#include "steam/session_store.h"
#include <atomic>
#include <functional>
#include <optional>
#include <string>

namespace z3cloudplay::steam {

enum class AuthError {
  kNone, kCanceled, kExpired, kRejected, kNetwork, kProtocol,
  kCredentialsRevoked, kPersistence, kNoSession
};

struct AuthResult {
  AuthError error = AuthError::kNone;
  std::optional<StoredSession> session;
};

// These callbacks run on the service worker. Challenge URLs are private auth
// presentation data; neither they nor credentials belong in diagnostics.
using ChallengeCallback = std::function<void(const std::string&)>;

class AuthBackend {
 public:
  virtual ~AuthBackend() = default;
  virtual AuthResult AuthenticateQr(const std::atomic<bool>& cancel,
                                    const ChallengeCallback& challenge) = 0;
  virtual AuthResult Restore(const StoredSession& session,
                             const std::atomic<bool>& cancel) = 0;
};

}  // namespace z3cloudplay::steam
