#pragma once
#include "steam/session_store.h"
#include <functional>
#include <string>

namespace z3cloudplay::security {
// A fail-closed replacement for the plaintext SessionStore.
// Provider must return a passphrase obtained interactively; implementations
// must never save it, log it or compile it into the executable.
class SecureSessionRepository final : public SessionRepository {
 public:
  using Unlock = std::function<std::string()>;
  SecureSessionRepository(Storage& storage, Unlock unlock)
      : storage_(storage), unlock_(std::move(unlock)) {}
  Result<std::optional<StoredSession>> Load() const override;
  Result<void> Save(const StoredSession& session) const override;
  Result<void> Clear() const override;
 private:
  Storage& storage_;
  Unlock unlock_;
};
}  // namespace z3cloudplay::security
