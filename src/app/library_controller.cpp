#include "app/library_controller.h"
#include <limits>

namespace z3cloudplay::app {
namespace {
const char* Message(const LibraryEvent& event) {
  using E=steam::LibraryError;
  switch(event.error) {
    case E::kCanceled:return "Library refresh canceled.";
    case E::kNetwork:return "Steam could not be reached. Retry to refresh.";
    case E::kProtocol:return "Steam returned an incomplete or invalid library. Retry to refresh.";
    case E::kCredentialsRevoked:return "Your Steam session needs renewal. Reconnect your account.";
    case E::kNoSession:return "Reconnect Steam to refresh your library.";
    case E::kPersistence:return event.cache_error==StorageError::kNotFound
        ? "No cached library is available. Connect to Steam to refresh."
        : "Saved library data is unavailable. Retry to refresh.";
    case E::kNone:return "";
  }
  return "Library refresh failed.";
}
}
LibraryController::LibraryController(homebrew::Simulation& model,LibraryService& service)
    :model_(model),service_(service) {}
void LibraryController::Loading() {
  model_.UpdateLibrary({{},true,{},model_.state().library_offline,model_.state().account.cache_available,{}});
}
void LibraryController::Activate(std::uint64_t account,bool offline) {
  Reset();account_=account;offline_=offline;needs_renewal_=false;
  if(!account_) return;
  Loading();
  request_=offline ? service_.LoadCached(account_,generation_) : service_.Refresh(account_,generation_);
  if(!request_) model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",model_.state().library_offline,model_.state().account.cache_available,{}});
}
void LibraryController::Refresh() {
  if(!account_ || clearing_) return;
  Loading();request_=service_.Refresh(account_,generation_);offline_=false;
  if(!request_) model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",model_.state().library_offline,model_.state().account.cache_available,{}});
}
void LibraryController::Reset() {
  service_.Invalidate(++generation_);request_=0;account_=0;clearing_=false;clear_result_.reset();offline_=false;needs_renewal_=false;
}
void LibraryController::Clear() {
  clear_result_.reset();clearing_=true;
  model_.UpdateLibrary({{},false,"Clearing saved library…",model_.state().library_offline,model_.state().account.cache_available,{}});
  if(!account_) { clear_result_=steam::LibraryError::kNone;clearing_=false;return; }
  request_=service_.Clear(account_,++generation_);
  if(!request_) { clear_result_=steam::LibraryError::kPersistence;clearing_=false; }
}
std::optional<steam::LibraryError> LibraryController::TakeClearResult() {
  auto value=clear_result_;clear_result_.reset();return value;
}
void LibraryController::Tick() {
  for(auto& event:service_.Pump(16)) {
    if(!request_ || event.request_id!=request_ || event.account_id!=account_ || event.account_generation!=generation_) continue;
    if(clearing_) {
      if(event.kind==LibraryEventKind::kCleared || event.kind==LibraryEventKind::kFailure) {
        clear_result_=event.kind==LibraryEventKind::kCleared ? steam::LibraryError::kNone : event.error;
        clearing_=false;request_=0;
        if(*clear_result_==steam::LibraryError::kNone)
          model_.UpdateLibrary({{},false,"Clearing saved session…",false,false,{}});
      }
      continue;
    }
    if(event.kind==LibraryEventKind::kLoading) { Loading();continue; }
    if(event.kind==LibraryEventKind::kFailure) {
      needs_renewal_=event.error==steam::LibraryError::kCredentialsRevoked || event.error==steam::LibraryError::kNoSession;
      model_.UpdateLibrary({{},false,Message(event),model_.state().account.cache_available || !model_.state().games.empty(),
                            event.cache_available,{}});
      request_=0;continue;
    }
    if(event.kind!=LibraryEventKind::kCached && event.kind!=LibraryEventKind::kComplete) continue;
    const auto time=event.retrieved_at_unix_seconds<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        ? std::optional<std::int64_t>(static_cast<std::int64_t>(event.retrieved_at_unix_seconds)) : std::nullopt;
    const bool cached=event.kind==LibraryEventKind::kCached;
    if(!model_.UpdateLibrary({std::move(event.games),false,{},cached,event.cache_available,time})) {
      const bool awaiting_fresh=cached && !offline_;
      model_.UpdateLibrary({{},awaiting_fresh,"Library data could not be displayed.",model_.state().library_offline,
                            model_.state().account.cache_available,{}});
      if(!awaiting_fresh) request_=0;
      continue;
    }
    if(cached && !offline_) Loading();
    else {
      const char* warning=event.cache_error!=StorageError::kNone ? "Library loaded, but saving failed. Retry to save."
          : event.metadata_partial ? "Some game details are unavailable." : "";
      if(*warning) model_.UpdateLibrary({{},false,warning,cached,event.cache_available,time});
      request_=0;
    }
  }
}
}
