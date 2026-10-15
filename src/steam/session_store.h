#pragma once

#include "platform/storage.h"

#include <cstdint>
#include <optional>
#include <string>

namespace z3cloudplay {

struct StoredSession {
  std::uint64_t steam_id = 0;
  std::string account_name;
  std::string refresh_token;
  std::string access_token;
  std::optional<std::uint32_t> cell_id;
};

class SessionRepository {
 public:
  virtual ~SessionRepository() = default;
  virtual Result<std::optional<StoredSession>> Load() const = 0;
  virtual Result<void> Save(const StoredSession& session) const = 0;
  virtual Result<void> Clear() const = 0;
};

class SessionStore final : public SessionRepository {
 public:
  explicit SessionStore(Storage& storage) : storage_(storage) {}

  Result<std::optional<StoredSession>> Load() const override;
  Result<void> Save(const StoredSession& session) const override;
  Result<void> Clear() const override;

 private:
  Storage& storage_;
};

}  // namespace z3cloudplay
