#include "app/persistence_controller.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace z3cloudplay;
using namespace z3cloudplay::app;
using namespace z3cloudplay::homebrew;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char value[] = "/tmp/z3cloudplay-persistence-controller-XXXXXX";
    const char* created = mkdtemp(value);
    assert(created);
    path_ = created;
  }
  ~TemporaryDirectory() {
    std::remove((path_ + "/preferences.conf.tmp").c_str());
    std::remove((path_ + "/preferences.conf").c_str());
    rmdir(path_.c_str());
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

class Repository final : public OrganizationRepository {
 public:
  mutable std::mutex mutex;
  mutable std::map<std::uint64_t, OrganizationSnapshot> snapshots;
  mutable std::atomic<int> loads{0}, saves{0}, clears{0};
  mutable std::atomic<bool> block_load{false}, load_entered{false}, release_load{false};
  StorageError load_error = StorageError::kNone;
  StorageError save_error = StorageError::kNone;
  StorageError clear_error = StorageError::kNone;

  Result<std::optional<OrganizationSnapshot>> Load(std::uint64_t account) const override {
    ++loads;
    load_entered = true;
    while (block_load && !release_load) std::this_thread::sleep_for(1ms);
    std::lock_guard<std::mutex> lock(mutex);
    if (load_error != StorageError::kNone)
      return Result<std::optional<OrganizationSnapshot>>::Failure(load_error, "load");
    const auto found = snapshots.find(account);
    return Result<std::optional<OrganizationSnapshot>>::Success(
        found == snapshots.end() ? std::optional<OrganizationSnapshot>{}
                                 : std::optional<OrganizationSnapshot>{found->second});
  }
  Result<void> Save(std::uint64_t account,
                    const OrganizationSnapshot& snapshot) const override {
    ++saves;
    if (save_error != StorageError::kNone)
      return Result<void>::Failure(save_error, "save");
    std::lock_guard<std::mutex> lock(mutex);
    snapshots[account] = snapshot;
    return Result<void>::Success();
  }
  Result<void> Clear(std::uint64_t account) const override {
    ++clears;
    if (clear_error != StorageError::kNone)
      return Result<void>::Failure(clear_error, "clear");
    std::lock_guard<std::mutex> lock(mutex);
    snapshots.erase(account);
    return Result<void>::Success();
  }
};

template <class Predicate>
void Wait(PersistenceController& controller, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    controller.Tick();
    if (predicate()) return;
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "persistence controller deadline exceeded");
}

void Connect(Simulation& model, PersistenceController& controller,
             std::uint64_t account = 123) {
  model.UpdateAccount({AuthState::SignedIn, std::to_string(account), "Synthetic", {}, {}, false});
  model.UpdateLibrary({{{42, "Synthetic game", {}, {}, {}, {}, {}}},
                       false, {}, false, false, {}});
  controller.Activate(account);
}

void Choose(Simulation& model, const std::string& action) {
  const auto& rows = model.view().rows;
  const auto found = std::find_if(rows.begin(), rows.end(),
                                  [&action](const ViewRow& row) {
                                    return row.action == action;
                                  });
  assert(found != rows.end());
  const int target = static_cast<int>(found - rows.begin());
  for (int step = 0; step < 80 && model.view().focused_row != target; ++step)
    assert(model.action(model.view().focused_row < target ? "down" : "up"));
  assert(model.view().focused_row == target);
  assert(model.action("cross"));
}

void OpenSettings(Simulation& model) {
  for (int step = 0; step < 4 && model.state().navigation.route != Route::Settings; ++step)
    assert(model.action("r1"));
  assert(model.state().navigation.route == Route::Settings);
}

void PendingLoadReplaysLocalEditAndSavesMergedSnapshot() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.favorites.insert(7);
  repository.snapshots[123] = stored;
  repository.block_load = true;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  while (!repository.load_entered) std::this_thread::sleep_for(1ms);

  assert(model.action("square"));
  assert(model.state().library.favorites.contains(42));
  repository.release_load = true;
  Wait(controller, [&] { return repository.saves.load() > 0; });

  assert(model.state().library.favorites == std::set<GameId>({7, 42}));
  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.at(123).favorites == std::set<GameId>({7, 42}));
}

void ClearFailureRetainsStateAndSuccessSuppressesOnlyUntilNextEdit() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.favorites.insert(42);
  repository.snapshots[123] = stored;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  Wait(controller, [&] { return model.state().library.favorites.contains(42); });

  repository.clear_error = StorageError::kRemoveFailed;
  controller.Clear();
  std::optional<StorageError> result;
  Wait(controller, [&] { result = controller.TakeClearResult(); return result.has_value(); });
  assert(*result == StorageError::kRemoveFailed);
  assert(model.state().library.favorites.contains(42));
  assert(model.state().persistence_available);
  controller.Retry();
  controller.Tick();
  assert(model.state().persistence_available);

  assert(model.action("select:42"));
  assert(model.action("square"));
  controller.Retry();
  Wait(controller, [&] { return repository.saves.load() == 1; });
  {
    std::lock_guard<std::mutex> lock(repository.mutex);
    assert(repository.snapshots.at(123).favorites.empty());
  }
  assert(model.action("square"));
  Wait(controller, [&] { return repository.saves.load() == 2; });

  repository.clear_error = StorageError::kNone;
  controller.Clear();
  Wait(controller, [&] { result = controller.TakeClearResult(); return result.has_value(); });
  assert(*result == StorageError::kNone);
  assert(model.state().library.favorites.contains(42));
  {
    std::lock_guard<std::mutex> lock(repository.mutex);
    assert(!repository.snapshots.contains(123));
  }

  assert(model.action("select:42"));
  assert(model.action("square"));
  Wait(controller, [&] { return repository.saves.load() == 3; });
  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.contains(123));
  assert(repository.snapshots.at(123).favorites.empty());
}

void ShutdownCapturesTheLastEdit() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  service.WaitIdle();
  controller.Tick();
  assert(model.action("select:42"));
  assert(model.action("square"));
  controller.FlushAndShutdown();

  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.at(123).favorites.contains(42));
}

void ShutdownWaitsForLoadBeforeMergingFinalEdit() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.notes[42] = "stored note";
  repository.snapshots[123] = stored;
  repository.block_load = true;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  while (!repository.load_entered) std::this_thread::sleep_for(1ms);
  assert(model.action("square"));

  std::thread release([&] {
    std::this_thread::sleep_for(20ms);
    repository.release_load = true;
  });
  controller.FlushAndShutdown();
  release.join();

  std::lock_guard<std::mutex> lock(repository.mutex);
  const auto& saved = repository.snapshots.at(123);
  assert(saved.notes.at(42) == "stored note");
  assert(saved.favorites.contains(42));
}

void InvalidMergedLoadStaysUnsavedUntilEditAndRetryResolveIt() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  for (GameId id = 100; id < 10100; ++id) stored.favorites.insert(id);
  assert(stored.favorites.size() == 10000);
  repository.snapshots[123] = stored;
  repository.block_load = true;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  while (!repository.load_entered) std::this_thread::sleep_for(1ms);
  assert(model.action("square"));
  repository.release_load = true;

  Wait(controller, [&] { return !model.state().persistence_available; });
  assert(repository.saves.load() == 0);
  assert(model.state().library.favorites == std::set<GameId>{42});

  assert(model.action("square"));
  controller.Retry();
  Wait(controller, [&] {
    return model.state().persistence_available && repository.saves.load() > 0;
  });
  assert(model.state().library.favorites.size() == 10000);
  assert(!model.state().library.favorites.contains(42));
}

void InitialPreferencesFailureRetriesWithoutOverwritingUnknownData() {
  TemporaryDirectory directory;
  const std::string blocked = directory.path() + "/blocked";
  std::FILE* file = std::fopen(blocked.c_str(), "wb");
  assert(file);
  std::fclose(file);
  Storage storage(blocked);
  PreferencesStore preferences(storage);
  Repository repository;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Wait(controller, [&] { return !model.state().persistence_available; });
  assert(access((blocked + "/preferences.conf").c_str(), F_OK) != 0);

  model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, false});
  OpenSettings(model);
  Choose(model, "toggle-motion");
  assert(model.state().reduced_motion);

  assert(std::remove(blocked.c_str()) == 0);
  assert(mkdir(blocked.c_str(), 0700) == 0);
  controller.Retry();
  Wait(controller, [&] { return model.state().persistence_available; });
  assert(model.state().reduced_motion);
  controller.FlushAndShutdown();
  assert(preferences.Load().preferences.reduce_motion);
  assert(std::remove((blocked + "/preferences.conf").c_str()) == 0);
  std::remove((blocked + "/preferences.conf.tmp").c_str());
  assert(rmdir(blocked.c_str()) == 0);
}

}  // namespace

int main() {
  PendingLoadReplaysLocalEditAndSavesMergedSnapshot();
  ClearFailureRetainsStateAndSuccessSuppressesOnlyUntilNextEdit();
  ShutdownCapturesTheLastEdit();
  ShutdownWaitsForLoadBeforeMergingFinalEdit();
  InvalidMergedLoadStaysUnsavedUntilEditAndRetryResolveIt();
  InitialPreferencesFailureRetriesWithoutOverwritingUnknownData();
}
