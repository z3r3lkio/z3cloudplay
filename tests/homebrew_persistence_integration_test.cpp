#include "homebrew/controller.hpp"
#include "app/persistence_service.h"
#include "library/cache.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace z3cloudplay;
using namespace z3cloudplay::homebrew;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;
namespace {
struct Sessions final : SessionRepository {
  mutable std::mutex mutex;
  mutable std::optional<StoredSession> value=StoredSession{123,"Synthetic","refresh","access",{}};
  Result<std::optional<StoredSession>> Load() const override {std::lock_guard<std::mutex> lock(mutex);return Result<std::optional<StoredSession>>::Success(value);}
  Result<void> Save(const StoredSession& s) const override {std::lock_guard<std::mutex> lock(mutex);value=s;return Result<void>::Success();}
  Result<void> Clear() const override {std::lock_guard<std::mutex> lock(mutex);value.reset();return Result<void>::Success();}
};
struct Auth final : AuthBackend {
  AuthResult AuthenticateQr(const std::atomic<bool>&,const ChallengeCallback&) override {return {AuthError::kCanceled,{}};}
  AuthResult Restore(const StoredSession& s,const std::atomic<bool>&) override {return {AuthError::kNone,s};}
};
struct Library final : LibraryBackend {
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {return {LibraryError::kNone,{{42,"Synthetic game",{},{},{}}},false};}
};
struct Organization final : OrganizationRepository {
  explicit Organization(Storage& storage):store(storage){}
  OrganizationStore store;
  std::atomic<bool> fail_clear{false},block_load{false},release{false};
  mutable std::atomic<bool> entered{false};
  Result<std::optional<OrganizationSnapshot>> Load(std::uint64_t account) const override {
    entered=true;
    while(block_load && !release)std::this_thread::sleep_for(1ms);
    return store.Load(account);
  }
  Result<void> Save(std::uint64_t account,const OrganizationSnapshot& snapshot) const override {return store.Save(account,snapshot);}
  Result<void> Clear(std::uint64_t account) const override {
    return fail_clear ? Result<void>::Failure(StorageError::kRemoveFailed,"synthetic failure") : store.Clear(account);
  }
};
template<class F> void Wait(Controller& controller,F predicate) {
  const auto until=std::chrono::steady_clock::now()+3s;
  do {controller.Tick();if(predicate())return;std::this_thread::sleep_for(1ms);}while(std::chrono::steady_clock::now()<until);
  assert(false && "persistence integration deadline");
}
void Choose(Simulation& model,const std::string& action) {
  const auto& rows=model.view().rows;
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const ViewRow& r){return r.action==action;});assert(found!=rows.end());
  const int index=static_cast<int>(found-rows.begin());
  for(int n=0;n<80 && model.view().focused_row!=index;++n)assert(model.action(model.view().focused_row<index?"down":"up"));
  assert(model.view().focused_row==index && model.action("cross"));
}
void Settings(Simulation& model) {
  for(int n=0;n<4 && model.state().navigation.route!=Route::Settings;++n)assert(model.action("r1"));
  assert(model.state().navigation.route==Route::Settings);
}
void SignOut(Simulation& model) {Settings(model);Choose(model,"signout-confirm");Choose(model,"confirm-destructive");}
void RestartAndDeletion(const std::filesystem::path& root) {
  Storage storage(root.string());PreferencesStore preferences(storage);Organization organization(storage);LibraryCache cache(storage);
  OrganizationSnapshot seed;seed.favorites.insert(42);seed.notes[42]="Saved note";seed.selected_game_id=42;
  assert(organization.Save(123,seed).ok());assert(preferences.Save({true}).ok());
  Sessions sessions;Auth auth;Library backend;
  {
    Simulation model({}, {},ModelMode::Live);
    LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);
    Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().games.size()==1 && model.state().library.favorites.contains(42) && model.state().reduced_motion;});
    assert(model.state().library.notes.at(42)=="Saved note");
    assert(model.action("r1"));if(model.state().navigation.library_rail_focused)assert(model.action("right"));
    assert(model.action("square"));
    Settings(model);Choose(model,"toggle-motion");
    controller.Shutdown();
    auto saved=organization.Load(123);assert(saved.ok() && saved.value() && saved.value()->favorites.empty());
    assert(saved.value()->notes.at(42)=="Saved note" && !preferences.Load().preferences.reduce_motion);
  }
  {
    Simulation model({}, {},ModelMode::Live);LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().library.notes.count(42)>0 && model.state().games.size()==1;});
    assert(model.state().library.favorites.empty() && !model.state().reduced_motion);
    organization.fail_clear=true;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedIn && !model.state().account.error.empty();});
    assert(sessions.Load().value() && organization.Load(123).value());
    assert(model.action("r1") && model.state().navigation.route==Route::Home);
    assert(model.action("square") && model.state().library.favorites.contains(42));
    Wait(controller,[&]{const auto saved=organization.Load(123);return saved.ok() && saved.value() && saved.value()->favorites.count(42)>0;});
    organization.fail_clear=false;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account.authentication==AuthState::SignedOut;});
    assert(!sessions.Load().value() && !organization.Load(123).value());
    assert(preferences.Load().status==PreferencesLoadStatus::kLoaded);
  }
}
void EditWhileLoading(const std::filesystem::path& root) {
  Storage storage(root.string());PreferencesStore preferences(storage);Organization organization(storage);LibraryCache cache(storage);
  OrganizationSnapshot seed;seed.favorites.insert(77);seed.notes[77]="Keep unrelated note";
  assert(organization.Save(123,seed).ok());organization.block_load=true;
  Sessions sessions;Auth auth;Library backend;Simulation model({}, {},ModelMode::Live);
  LibraryService library(backend,sessions,&cache);app::PersistenceService persistence(preferences,organization);
  Controller controller(model,auth,sessions,&library,&persistence);
  Wait(controller,[&]{return organization.entered && model.state().games.size()==1;});
  assert(model.action("r1"));if(model.state().navigation.library_rail_focused)assert(model.action("right"));
  assert(model.action("square") && model.state().library.favorites.contains(42));
  organization.release=true;
  Wait(controller,[&]{return model.state().library.favorites.contains(77) && model.state().library.favorites.contains(42);});
  controller.Shutdown();
  const auto loaded=organization.Load(123);
  assert(loaded.ok() && loaded.value() && loaded.value()->favorites==std::set<GameId>({42,77}));
  assert(loaded.value()->notes.at(77)=="Keep unrelated note");
}

}
int main() {
  const auto root=std::filesystem::temp_directory_path()/("z3cloudplay-persistence-integration-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);RestartAndDeletion(root);EditWhileLoading(root/"delayed");std::filesystem::remove_all(root);
}
