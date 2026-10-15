#include "homebrew/controller.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

using namespace z3cloudplay;
using namespace z3cloudplay::homebrew;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;
namespace {
StoredSession Session() { return {12345,"Synthetic account","fixture-refresh-secret","fixture-access-secret",{}}; }
class Repository final : public SessionRepository {
 public:
  mutable std::mutex mutex;
  mutable std::optional<StoredSession> value;
  mutable int clears = 0;
  std::atomic<bool> fail_load{false};
  std::atomic<bool> fail_clear{false};
  Result<std::optional<StoredSession>> Load() const override {
    std::lock_guard<std::mutex> lock(mutex);
    if (fail_load) return Result<std::optional<StoredSession>>::Failure(StorageError::kReadFailed,"fixture failure");
    return Result<std::optional<StoredSession>>::Success(value);
  }
  Result<void> Save(const StoredSession& s) const override {
    std::lock_guard<std::mutex> lock(mutex); value=s; return Result<void>::Success();
  }
  Result<void> Clear() const override {
    std::lock_guard<std::mutex> lock(mutex); ++clears;
    if (fail_clear) return Result<void>::Failure(StorageError::kRemoveFailed,"fixture failure");
    value.reset(); return Result<void>::Success();
  }
};
class Backend final : public AuthBackend {
 public:
  std::atomic<int> calls{0};
  std::atomic<bool> approve{false};
  std::atomic<bool> replace{false};
  bool ignore_cancel = false;
  AuthError error = AuthError::kNone;
  AuthResult AuthenticateQr(const std::atomic<bool>& cancel,const ChallengeCallback& cb) override {
    ++calls; cb("https://s.team/q/synthetic-first");
    bool sent = false;
    const auto deadline = std::chrono::steady_clock::now()+3s;
    while (!approve && (ignore_cancel || !cancel) && std::chrono::steady_clock::now()<deadline) {
      if (replace && !sent) { sent=true; cb("https://s.team/q/synthetic-replaced"); }
      std::this_thread::sleep_for(1ms);
    }
    if (cancel && !ignore_cancel) return {AuthError::kCanceled,{}};
    return {error,error == AuthError::kNone ? std::optional<StoredSession>(Session()) : std::nullopt};
  }
  AuthResult Restore(const StoredSession& s,const std::atomic<bool>&) override { return {error,error == AuthError::kNone ? std::optional<StoredSession>(s) : std::nullopt}; }
};
class LibraryBackendFixture final : public LibraryBackend {
 public:
  std::atomic<int> calls{0};
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {
    ++calls;return {LibraryError::kNone,{{42,"Connected synthetic game",{},{},{}}},false};
  }
};
class CacheFixture final : public LibraryRepository {
 public:
  mutable std::optional<LibrarySnapshot> value;
  bool fail_clear=false;
  Result<LibrarySnapshot> Load(std::uint64_t) const override {
    return value ? Result<LibrarySnapshot>::Success(*value)
                 : Result<LibrarySnapshot>::Failure(StorageError::kNotFound,"missing");
  }
  Result<void> Save(std::uint64_t,const LibrarySnapshot& snapshot) const override {value=snapshot;return Result<void>::Success();}
  Result<void> Clear(std::uint64_t) const override {
    if(fail_clear)return Result<void>::Failure(StorageError::kRemoveFailed,"failure");
    value.reset();return Result<void>::Success();
  }
};
void Wait(Controller& controller,const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now()+4s;
  do { controller.Tick(); if (predicate()) return; std::this_thread::sleep_for(1ms); } while (std::chrono::steady_clock::now()<deadline);
  assert(false && "controller event deadline exceeded");
}
void Choose(Simulation& model,const std::string& action) {
  const auto& rows=model.view().rows;
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const ViewRow& row){return row.action==action;});
  assert(found!=rows.end());
  const auto index=static_cast<int>(found-rows.begin());
  for (int i=0; i<80 && model.view().focused_row!=index; ++i)
    assert(model.action(model.view().focused_row<index?"down":"up"));
  assert(model.view().focused_row==index); assert(model.action("cross"));
}
void OpenSettings(Simulation& model) {
  for (int i=0; i<4 && model.state().navigation.route!=Route::Settings; ++i) model.action("r1");
  assert(model.state().navigation.route==Route::Settings);
}
void SignOut(Simulation& model) { OpenSettings(model); Choose(model,"signout-confirm"); Choose(model,"confirm-destructive"); }
void Connect(Simulation& model) { Choose(model,"connect"); }
void ChallengeAndCancel() {
  Simulation model({}, {},ModelMode::Live); Repository repo; Backend backend; backend.ignore_cancel=true;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedOut;});
  assert(model.state().games.empty() && backend.calls==0);
  Connect(model); Wait(controller,[&]{return !model.state().account.challenge_url.empty();});
  backend.replace=true;
  Wait(controller,[&]{return model.state().account.challenge_url.find("replaced")!=std::string::npos;});
  assert(model.snapshot().find("s.team")==std::string::npos);
  Choose(model,"qr-cancel"); controller.Tick(); backend.approve=true;
  Wait(controller,[&]{return model.state().account.authentication==AuthState::Cancelled;});
  controller.Shutdown();
  assert(!repo.Load().value() && model.state().account.challenge_url.empty());
}
void AuthenticationRestoreAndSignOut() {
  Repository repo;
  {
    Simulation model({}, {},ModelMode::Live); Backend backend; Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedOut;});
    Connect(model); Wait(controller,[&]{return !model.state().account.challenge_url.empty();});
    backend.approve=true;
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    assert(model.state().account.id=="12345" && model.state().account.challenge_url.empty());
    assert(model.state().games.empty() && model.state().jobs.empty());
    assert(!model.state().account.cache_available);
    assert(model.action("account-sync"));
    controller.Tick();
    assert(model.state().account.authentication==AuthState::SignedIn);
    assert(!model.state().library_syncing && !model.state().library_loading && !model.state().account.error.empty());
    assert(model.snapshot().find("fixture-refresh-secret")==std::string::npos);
    assert(model.snapshot().find("Synthetic account")==std::string::npos);
  }
  {
    Simulation model({}, {},ModelMode::Live); Backend backend; Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    assert(backend.calls==0);
    SignOut(model); controller.Tick();
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedOut;});
    assert(!repo.Load().value() && model.state().account.id.empty());
  }
}
void FailedClearDoesNotSwitch() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_clear=true;
  Backend backend; Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
  OpenSettings(model); Choose(model,"account"); Choose(model,"account-switch"); Choose(model,"confirm-destructive");
  controller.Tick(); Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn && !model.state().account.error.empty();});
  assert(backend.calls==0 && repo.Load().value() && !model.state().account.error.empty());
  assert(model.state().account.id=="12345" && model.state().account.display_name=="Synthetic account");
  assert(model.state().navigation.route==Route::Settings);
  assert(model.view().subtitle.find("Sign-out failed")!=std::string::npos);
  assert(std::none_of(model.view().rows.begin(),model.view().rows.end(),[](const ViewRow& row){return row.action=="qr-retry";}));
  repo.fail_clear=false;
  SignOut(model); controller.Tick();
  Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedOut;});
  assert(backend.calls==0 && !repo.Load().value());
}
void SignInAgainKeepsTheAccount() {
  // Approved: a new token is saved and nothing account-specific is cleared.
  {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    OpenSettings(model); Choose(model,"account"); Choose(model,"account-reauth");
    Wait(controller,[&]{return model.state().account.authentication==AuthState::Waiting && !model.state().account.challenge_url.empty();});
    assert(model.state().navigation.route==Route::SignIn && model.state().account.id=="12345");
    backend.approve=true;
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    assert(backend.calls==1 && repo.clears==0 && repo.Load().value() && model.state().account.id=="12345");
  }
  // Cancelled: back to the saved sign-in.
  {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    OpenSettings(model); Choose(model,"account"); Choose(model,"account-reauth");
    Wait(controller,[&]{return model.state().account.authentication==AuthState::Waiting && !model.state().account.challenge_url.empty();});
    Choose(model,"qr-cancel"); controller.Tick();
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    assert(repo.clears==0 && repo.Load().value() && model.state().account.id=="12345");
  }
}
void ExitHonorsClearWithoutStartingNewQr() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
  OpenSettings(model); Choose(model,"account"); Choose(model,"account-switch"); Choose(model,"confirm-destructive");
  controller.Shutdown();
  assert(backend.calls==0 && !repo.Load().value());
}
void StorageFailureRetriesSavedSession() {
  for (bool manual : {true,false}) {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_load=true;
    Backend backend; Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::Failed;});
    assert(model.state().account.challenge_url.empty());
    assert(model.view().subtitle.find("sign out")==std::string::npos);
    assert(backend.calls==0 && repo.clears==0);
    repo.fail_load=false;
    if (manual) Choose(model,"qr-retry");
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
    assert(backend.calls==0 && repo.clears==0);
    assert(model.state().account.id=="12345" && model.state().account.challenge_url.empty());
    assert(repo.Load().value()->refresh_token==Session().refresh_token);
  }
}
void StorageFailureKeepsVisibleAccount() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session();
  Backend backend; backend.error=AuthError::kNetwork;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account.authentication==AuthState::Offline;});
  model.UpdateLibrary({{{42,"Cached game",{},{},{}}},false,{},true,true,{}});
  repo.fail_load=true;
  assert(model.action("connect"));
  Wait(controller,[&]{return model.state().account.authentication==AuthState::Failed;});
  assert(model.state().account.id=="12345" && model.state().games.size()==1);
  assert(model.state().account.challenge_url.empty() && backend.calls==0 && repo.clears==0);
  backend.error=AuthError::kNone; repo.fail_load=false;
  Choose(model,"qr-retry");
  Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
  assert(model.state().account.id=="12345" && model.state().games.size()==1);
  assert(backend.calls==0 && repo.clears==0);
}
void CanceledStorageRetryKeepsSavedSessionIntent() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_load=true;
  Backend backend; Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account.authentication==AuthState::Failed;});
  assert(model.action("circle")); controller.Tick();
  repo.fail_load=false;
  const auto deadline=std::chrono::steady_clock::now()+2200ms;
  while (std::chrono::steady_clock::now()<deadline) {
    controller.Tick();
    assert(model.state().account.authentication!=AuthState::SignedIn && backend.calls==0);
    std::this_thread::sleep_for(10ms);
  }
  assert(model.action("connect"));
  Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn;});
  assert(backend.calls==0 && repo.clears==0 && repo.Load().value());
}
void RevokedSessionCleanupRetriesAfterStorageRecovery() {
  for (bool manual : {true, false}) {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_clear=true;
    Backend backend; backend.error=AuthError::kCredentialsRevoked;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::Failed;});
    assert(repo.Load().value() && backend.calls==0);
    repo.fail_clear=false;
    if (manual) Choose(model,"qr-retry");
    Wait(controller,[&]{return !model.state().account.challenge_url.empty();});
    assert(model.state().account.authentication==AuthState::Waiting);
    assert(!repo.Load().value() && backend.calls==1);
    controller.Shutdown();
  }
}
void StorageFailureAndRevocation() {
  for (const bool corrupt : {false,true}) {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_load=corrupt;
    Backend backend; backend.error=AuthError::kCredentialsRevoked;
    Controller controller(model,backend,repo);
    if (corrupt) {
      Wait(controller,[&]{return model.state().account.authentication==AuthState::Failed;});
      assert(model.state().account.challenge_url.empty() && model.state().account.id.empty());
    } else {
      // A revoked session shows a new QR code right away.
      Wait(controller,[&]{return !model.state().account.challenge_url.empty();});
      assert(model.state().account.authentication==AuthState::Waiting && model.state().account.id.empty());
    }
    controller.Shutdown();
    std::lock_guard<std::mutex> lock(repo.mutex); assert(corrupt?repo.value.has_value():!repo.value.has_value());
  }
  {
    // Steam refuses the saved session (eresult 63 on the console): a new QR code, not a failure.
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session();
    Backend backend; backend.error=AuthError::kRejected;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return !model.state().account.challenge_url.empty();});
    assert(model.state().account.authentication==AuthState::Waiting && backend.calls==1);
    controller.Shutdown();
  }
}
void ConnectedLibraryAndOfflineRestart() {
  Repository repo;repo.value=Session();CacheFixture cache;
  {
    Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
    assert(model.state().account.authentication==AuthState::SignedIn && library_backend.calls==1);
    assert(model.state().games[0].id==42 && model.state().account.cache_available);
  }
  {
    Simulation model({}, {},ModelMode::Live);Backend backend;backend.error=AuthError::kNetwork;
    LibraryBackendFixture library_backend;LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    Wait(controller,[&]{return model.state().account.authentication==AuthState::Offline && model.state().games.size()==1 && !model.state().library_loading;});
    assert(library_backend.calls==0 && model.state().library_offline);
    cache.fail_clear=true;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account.authentication==AuthState::Offline && !model.state().account.error.empty();});
    assert(repo.Load().value() && repo.clears==0 && cache.value && model.state().games.size()==1);
    cache.fail_clear=false;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedOut;});
    assert(!repo.Load().value() && !cache.value && model.state().games.empty());
  }
}
void RepeatedClearPreservesFirstIntent() {
  for(bool switching:{false,true}) {
    Repository repo;repo.value=Session();CacheFixture cache;
    Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
    assert(model.action(switching?"account-switch":"signout"));
    assert(model.action(switching?"signout":"account-switch"));
    controller.Tick();
    Wait(controller,[&]{return switching ? !model.state().account.challenge_url.empty()
                                        : model.state().account.authentication==AuthState::SignedOut;});
    assert(repo.clears==1 && !cache.value && backend.calls==(switching?1:0));
  }
}
void ShutdownClearsLibraryBeforeSession() {
  Repository repo;repo.value=Session();CacheFixture cache;
  Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
  LibraryService library_service(library_backend,repo,&cache);
  Controller controller(model,backend,repo,&library_service);
  Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
  SignOut(model);controller.Shutdown();
  assert(!repo.Load().value() && !cache.value);
}
}
int main() { CanceledStorageRetryKeepsSavedSessionIntent(); RevokedSessionCleanupRetriesAfterStorageRecovery(); StorageFailureKeepsVisibleAccount(); StorageFailureRetriesSavedSession(); ChallengeAndCancel(); AuthenticationRestoreAndSignOut(); FailedClearDoesNotSwitch(); SignInAgainKeepsTheAccount(); ExitHonorsClearWithoutStartingNewQr(); StorageFailureAndRevocation(); ConnectedLibraryAndOfflineRestart(); ShutdownClearsLibraryBeforeSession(); RepeatedClearPreservesFirstIntent(); }
