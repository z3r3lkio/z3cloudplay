#pragma once
#include "steam/session_store.h"
#include <mutex>
#include <optional>
namespace z3cloudplay::security {
// Default credential policy for PS5 until a secure masked unlock UI exists.
// Session tokens never touch filesystem storage. Re-login via Steam QR after restart.
class EphemeralSessionRepository final : public SessionRepository {
 public:
  Result<std::optional<StoredSession>> Load() const override;
  Result<void> Save(const StoredSession& session) const override;
  Result<void> Clear() const override;
  ~EphemeralSessionRepository() override { Clear(); }
 private:
  mutable std::mutex mutex_;
  mutable std::optional<StoredSession> session_;
};
}  // namespace z3cloudplay::security
