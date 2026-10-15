#pragma once
#include "steam/session_store.h"
#include <mutex>
#include <optional>
#include <string>
namespace z3cloudplay::security {
class EphemeralSessionRepository final : public SessionRepository {
 public:
  ~EphemeralSessionRepository() override { Clear(); }
  Result<std::optional<StoredSession>> Load() const override {
    std::lock_guard<std::mutex> guard(mutex_);
    return Result<std::optional<StoredSession>>::Success(session_);
  }
  Result<void> Save(const StoredSession& value) const override {
    if(!value.steam_id || value.refresh_token.empty() ||
       value.account_name.size()>256 || value.refresh_token.size()>24576 ||
       value.access_token.size()>24576)
      return Result<void>::Failure(StorageError::kInvalidData,"Invalid volatile session");
    std::lock_guard<std::mutex> guard(mutex_);
    Wipe();
    session_=value;
    return Result<void>::Success();
  }
  Result<void> Clear() const override {
    std::lock_guard<std::mutex> guard(mutex_);
    Wipe();
    return Result<void>::Success();
  }
 private:
  static void Zero(std::string& s) {
    if(!s.empty()) {
      volatile char* ptr=&s[0];
      for(std::size_t i=0;i<s.size();++i) ptr[i]=0;
      s.clear();
    }
  }
  void Wipe() const {
    if(session_) {
      Zero(session_->refresh_token);
      Zero(session_->access_token);
      Zero(session_->account_name);
      session_.reset();
    }
  }
  mutable std::mutex mutex_;
  mutable std::optional<StoredSession> session_;
};
}  // namespace z3cloudplay::security
