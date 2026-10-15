#pragma once
#include "steam/session_store.h"
#include <functional>

namespace z3cloudplay {
// Lazy filesystem preparation runs only on the calling service worker. The
// configured root's parent must already exist; failures never enable fixtures.
class LocalSessionRepository final : public SessionRepository {
 public:
  using DiagnosticReport = std::function<void(const std::string&)>;
  // Reports operation/error metadata only; never session bytes or account data.
  explicit LocalSessionRepository(std::string root, DiagnosticReport report = {});
  Result<std::optional<StoredSession>> Load() const override;
  Result<void> Save(const StoredSession& session) const override;
  Result<void> Clear() const override;
 private:
  Result<void> Prepare() const;
  std::string root_;
  mutable Storage storage_;
  SessionStore store_;
  mutable bool prepared_ = false;
  DiagnosticReport report_;
};
}
