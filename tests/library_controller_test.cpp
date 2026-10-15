#include "app/library_controller.h"
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>

using namespace z3cloudplay;
using namespace z3cloudplay::homebrew;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;
namespace {
struct Sessions final : SessionRepository {
  mutable std::mutex mutex;
  std::optional<StoredSession> value=StoredSession{123,"Synthetic","refresh","access",{}};
  Result<std::optional<StoredSession>> Load() const override { std::lock_guard<std::mutex> lock(mutex); return Result<std::optional<StoredSession>>::Success(value); }
  Result<void> Save(const StoredSession&) const override { assert(false); return Result<void>::Success(); }
  Result<void> Clear() const override { assert(false); return Result<void>::Success(); }
};
struct Cache final : LibraryRepository {
  mutable std::optional<LibrarySnapshot> value;
  mutable std::uint64_t owner=123;
  bool fail_clear=false;
  Result<LibrarySnapshot> Load(std::uint64_t id) const override {
    if(!value || id!=owner) return Result<LibrarySnapshot>::Failure(StorageError::kNotFound,"missing");
    return Result<LibrarySnapshot>::Success(*value);
  }
  Result<void> Save(std::uint64_t id,const LibrarySnapshot& snapshot) const override { owner=id;value=snapshot;return Result<void>::Success(); }
  Result<void> Clear(std::uint64_t id) const override {
    if(fail_clear) return Result<void>::Failure(StorageError::kRemoveFailed,"failure");
    if(owner==id) value.reset(); return Result<void>::Success();
  }
};
struct Backend final : LibraryBackend {
  std::atomic<bool> block{false},release{false};
  std::atomic<int> calls{0};
  LibraryResult result{LibraryError::kNone,{{42,"Synthetic title",{},{},{}}},false};
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>& cancel) override {
    ++calls;
    while(block && !release && !cancel) std::this_thread::sleep_for(1ms);
    return result;
  }
};
template<class Predicate> void Wait(app::LibraryController& controller,Predicate predicate) {
  const auto deadline=std::chrono::steady_clock::now()+2s;
  do { controller.Tick(); if(predicate()) return; std::this_thread::sleep_for(1ms); } while(std::chrono::steady_clock::now()<deadline);
  assert(false && "library presentation deadline");
}
void FreshRefreshAndFailure() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  LibraryService service(backend,sessions,&cache,[]{return 12345;});
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==1;});
  assert(model.state().games[0].id==42 && model.state().account.cache_available);
  assert(model.state().account.last_sync==12345 && model.state().account.authentication==AuthState::SignedIn);
  backend.result={LibraryError::kNetwork,{},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && !model.state().account.error.empty();});
  assert(model.state().games.size()==1 && model.state().library_offline);
  backend.result={LibraryError::kNone,{},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.empty();});
  assert(model.state().account.error.empty() && model.state().account.cache_available);
}
void OfflineCacheAndClear() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::Offline,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  cache.value=LibrarySnapshot{};cache.value->retrieved_at_unix_seconds=100;
  cache.value->games={{7,"Offline title",{},{},{}}};
  LibraryService service(backend,sessions,&cache);
  app::LibraryController controller(model,service);
  controller.Activate(123,true);
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==1;});
  assert(backend.calls==0 && model.state().library_offline && model.state().account.authentication==AuthState::Offline);
  cache.fail_clear=true; controller.Clear();
  std::optional<LibraryError> cleared;
  Wait(controller,[&]{cleared=controller.TakeClearResult();return cleared.has_value();});
  assert(*cleared==LibraryError::kPersistence && model.state().games.size()==1);
  cache.fail_clear=false;controller.Clear();
  Wait(controller,[&]{cleared=controller.TakeClearResult();return cleared.has_value();});
  assert(*cleared==LibraryError::kNone && !cache.value);
}
void RejectedCacheStillAcceptsFreshResult() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  cache.value=LibrarySnapshot{};
  cache.value->games={{7,"   ",{},{},{}}};
  backend.block=true;
  LibraryService service(backend,sessions,&cache);
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return backend.calls>0;});
  controller.Tick();backend.release=true;
  Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
  assert(model.state().games.front().id==42);
}
void RejectedRequestStopsLoading() {
  Simulation model({}, {},ModelMode::Live);
  Sessions sessions;Cache cache;Backend backend;
  LibraryService service(backend,sessions,&cache);service.Shutdown();
  app::LibraryController controller(model,service);controller.Activate(123,false);
  assert(!model.state().library_loading && !model.state().account.error.empty());
}
void StaleCompletionCannotRepopulate() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;backend.block=true;
  LibraryService service(backend,sessions,&cache);
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return backend.calls>0;});
  controller.Reset();
  model.UpdateAccount({AuthState::SignedOut,{},{},{},{},true});
  backend.release=true;service.Shutdown();controller.Tick();
  assert(model.state().games.empty() && model.state().account.id.empty());
}
}
int main(){FreshRefreshAndFailure();OfflineCacheAndClear();StaleCompletionCannotRepopulate();RejectedCacheStillAcceptsFreshResult();RejectedRequestStopsLoading();}
