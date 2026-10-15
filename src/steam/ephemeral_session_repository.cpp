#include "steam/ephemeral_session_repository.h"
#include <algorithm>
#include <utility>
namespace z3cloudplay::security {
namespace {
void ClearString(std::string& s) {
  if (!s.empty()) {
    volatile char* data = &s[0];
    for(std::size_t i=0;i<s.size();++i) data[i]=0;
    s.clear();
  }
}
void Erase(std::optional<StoredSession>& s) {
  if(s) {
    ClearString(s->refresh_token);
    ClearString(s->access_token);
    ClearString(s->account_name);
    s.reset();
  }
}
}
Result<std::optional<StoredSession>> EphemeralSessionRepository::Load() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return Result<std::optional<StoredSession>>::Success(session_);
}
Result<void> EphemeralSessionRepository::Save(const StoredSession& s) const {
  if(s.steam_id==0 || s.refresh_token.empty() || s.account_name.size()>256 ||
     s.refresh_token.size()>24*1024 || s.access_token.size()>24*1024)
    return Result<void>::Failure(StorageError::kInvalidData,"Invalid in-memory Steam session");
  std::lock_guard<std::mutex> guard(mutex_);
  Erase(session_);
  session_=s;
  return Result<void>::Success();
}
Result<void> EphemeralSessionRepository::Clear() const {
  std::lock_guard<std::mutex> guard(mutex_);
  Erase(session_);
  return Result<void>::Success();
}
}  // namespace z3cloudplay::security
