#include "runtime/game_install.h"

#include "platform/storage.h"

#include <cstdio>
#include <set>
#include <cerrno>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace z3cloudplay::runtime;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
enum class SyncFault { kNone, kFile, kDirectory };
SyncFault sync_fault = SyncFault::kNone;
}
extern "C" int fsync(int fd) {
  struct stat info{};
  if (fstat(fd, &info) == 0 &&
      ((sync_fault == SyncFault::kFile && S_ISREG(info.st_mode)) ||
       (sync_fault == SyncFault::kDirectory && S_ISDIR(info.st_mode)))) {
    errno = EIO;
    return -1;
  }
  static const auto actual = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
  if (!actual) { errno = ENOSYS; return -1; }
  return actual(fd);
}

namespace {

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "z3cloudplay-runtime-install-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char* made = ::mkdtemp(writable.data());
    CHECK(made != nullptr);
    path = std::filesystem::canonical(made).string();
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::string path;
};

ReserveRequest Request(std::string install_id = "install-hl2") {
  ReserveRequest request;
  request.install_id = std::move(install_id);
  request.app_id = 220;
  request.default_profile_id = "game";
  request.runtime_bundle_id = "prospero-win-e16ec8f";
  request.recipe_id = "steam-220-hl2-v1";
  request.recipe_version = "1";
  request.settings_revision = std::numeric_limits<std::uint64_t>::max();
  request.launch_entry_id = "0";
  request.launch_path = "hl2.exe";
  request.launch_arguments = "-game hl2 -steam";
  request.launch_working_directory = "";
  request.install_path = "/mnt/ext1/z3cloudplay/games/220";
  request.volume_id = "m2";
  request.kit_title_variants = {"FAKE78000", "FAKE78001", "FAKE78002"};
  return request;
}

NativeTitleAvailability Free(const std::string&) {
  return {TitleAvailability::kFree, TitleAvailability::kFree};
}

void ReservationSurvivesRestartAndReinstallReusesIdentity() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());

  RuntimeInstallRepository first(storage, "runtime-installs.json", Free);
  CHECK(first.Load().ok());
  auto reserved = first.Reserve(Request());
  CHECK(reserved.ok());
  CHECK(reserved.value().title_id == "FAKE78000");
  CHECK(reserved.value().data_root == "/data/z3cloudplay/games/install-hl2");
  CHECK(reserved.value().exit_policy == "exit-title");

  RuntimeInstallRepository restarted(
      storage, "runtime-installs.json", [](const std::string&) {
        return NativeTitleAvailability{TitleAvailability::kOccupied,
                                       TitleAvailability::kOccupied};
      });
  CHECK(restarted.Load().ok());
  const auto found = restarted.Find("install-hl2");
  CHECK(found && found->title_id == "FAKE78000");
  CHECK(found->settings_revision == std::numeric_limits<std::uint64_t>::max());
  CHECK(found->launch_entry_id == "0" && found->launch_arguments == "-game hl2 -steam");
  auto reinstall = Request();
  reinstall.kit_title_variants = {"FAKE78002"};
  const auto reused = restarted.Reserve(reinstall);
  CHECK(reused.ok() && reused.value().title_id == "FAKE78000");
  CHECK(restarted.Snapshot().size() == 1);
}

void NativeInventoryAndSourceCollisionsAreNeverAllocated() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  std::unordered_map<std::string, NativeTitleAvailability> availability{
      {"FAKE78000", {TitleAvailability::kOccupied, TitleAvailability::kFree}},
      {"FAKE78001", {TitleAvailability::kFree, TitleAvailability::kUnknown}},
      {"FAKE78002", {TitleAvailability::kUnknown, TitleAvailability::kFree}},
      {"FAKE78003", {TitleAvailability::kFree, TitleAvailability::kFree}},
  };
  RuntimeInstallRepository repository(
      storage, "runtime-installs.json", [&](const std::string& title) {
        return availability.at(title);
      });
  CHECK(repository.Load().ok());
  auto collision_request = Request("collision-test");
  collision_request.kit_title_variants.push_back("FAKE78003");
  const auto reserved = repository.Reserve(collision_request);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78003");

  auto exhausted = Request("exhausted-test");
  exhausted.kit_title_variants = {"PPSA99995", "FAKE77031", "FAKE78000", "FAKE78001",
                                  "FAKE78002", "FAKE78003"};
  const auto rejected = repository.Reserve(exhausted);
  CHECK(!rejected.ok() && rejected.error() == RepositoryError::kTitleVariantsExhausted);
}

void LegacyDownloadAdoptsWithoutCopyAndRemainsDownloaded() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());

  LegacyInstall request;
  request.install_id = "legacy-hl2";
  request.app_id = 220;
  request.install_path = "/data/homebrew/FAKE77031/games/220";
  request.volume_id = "internal";
  request.launch_path = "hl2.exe";
  request.launch_entry_id = "0";
  request.launch_arguments = "-game hl2 -steam";
  request.default_profile_id = "game";
  request.runtime_bundle_id = "prospero-win-e16ec8f";
  request.recipe_id = "steam-220-hl2-v1";
  request.recipe_version = "1";
  request.kit_title_variants = {"FAKE78003"};
  const auto adopted = repository.Adopt(request);
  CHECK(adopted.ok());
  CHECK(adopted.value().state == PreparationState::kDownloaded);
  CHECK(adopted.value().install_path == "/data/homebrew/FAKE77031/games/220");
  CHECK(!std::filesystem::exists(std::filesystem::path(directory.path) / "data" / "z3cloudplay" /
                                 "games" / "legacy-hl2"));
}

void StateArtifactsFailureAndJournalRoundTrip() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  auto reserved = repository.Reserve(Request("stateful"));
  CHECK(reserved.ok());
  auto record = reserved.value();
  record.state = PreparationState::kRemoving;
  record.artifacts = {
      {"runtime", "e16ec8f", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
      {"helper", "e9f2cfc", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"}};
  record.last_failure = RuntimeFailure{"registration-busy", "native service busy"};
  record.journal = PreparationJournal{"remove-1", "unregister", "source-retired"};
  CHECK(repository.Update(record).ok());
  auto invalid_hash = record;
  invalid_hash.artifacts[0].sha256 = "short";
  CHECK(repository.Update(invalid_hash).error() == RepositoryError::kInvalidRecord);
  auto invalid_version = record;
  invalid_version.recipe_version = "../unversioned";
  CHECK(repository.Update(invalid_version).error() == RepositoryError::kInvalidRecord);
  auto reassigned = record;
  reassigned.title_id = "FAKE78999";
  CHECK(repository.Update(reassigned).error() == RepositoryError::kConflict);

  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  const auto loaded = restarted.Find("stateful");
  CHECK(loaded && loaded->state == PreparationState::kRemoving);
  CHECK(loaded->artifacts.size() == 2 && loaded->artifacts[1].identity == "e9f2cfc");
  CHECK(loaded->last_failure && loaded->last_failure->code == "registration-busy");
  CHECK(loaded->journal && loaded->journal->step == "source-retired");

  auto contender = Request("contender");
  contender.kit_title_variants = {loaded->title_id};
  CHECK(restarted.Reserve(contender).error() == RepositoryError::kTitleVariantsExhausted);
  bool source_free = false;
  RuntimeInstallRepository removal_repository(
      storage, "runtime-installs.json", [&](const std::string&) {
        return NativeTitleAvailability{TitleAvailability::kFree,
                                       source_free ? TitleAvailability::kFree
                                                   : TitleAvailability::kOccupied};
      });
  CHECK(removal_repository.Load().ok());
  CHECK(removal_repository.EraseAfterUnregister("stateful").error() ==
        RepositoryError::kRemovalIncomplete);
  CHECK(removal_repository.Find("stateful"));
  source_free = true;
  CHECK(removal_repository.EraseAfterUnregister("stateful").ok());
  CHECK(!removal_repository.Find("stateful"));
  CHECK(removal_repository.Reserve(contender).ok());
}

// With one shell for every game the candidates are the whole FAKE78001-FAKE78999 range
// (runtime::KitTitleCandidates): more than ten games each get the lowest free title, titles
// taken on the console are skipped, games kept on old fixed titles keep them, a removed game's
// title is used again, and a title reserved before a stop stays with its game after a restart.
void AnyTitleRangeAllocatesBeyondTenAndReusesRemovedTitles() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  std::vector<std::string> range;
  for (int number = 1; number <= 999; ++number) {
    char title[16];
    std::snprintf(title, sizeof(title), "FAKE78%03d", number);
    range.push_back(title);
  }
  // FAKE78003 is someone else's folder; FAKE78004 is still listed by the console.
  const auto availability = [](const std::string& title) {
    if (title == "FAKE78003") return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kOccupied};
    if (title == "FAKE78004") return NativeTitleAvailability{TitleAvailability::kOccupied, TitleAvailability::kFree};
    return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
  };
  RuntimeInstallRepository repository(storage, "runtime-installs.json", availability);
  CHECK(repository.Load().ok());
  // A game prepared with an older ten-title kit keeps its fixed title.
  auto fixed = Request("old-fixed");
  fixed.kit_title_variants = {"FAKE78001"};
  CHECK(repository.Reserve(fixed).ok());

  std::vector<std::string> titles;
  for (int game = 0; game < 12; ++game) {
    auto request = Request("game-" + std::to_string(game));
    request.kit_title_variants = range;
    const auto reserved = repository.Reserve(request);
    CHECK(reserved.ok());
    titles.push_back(reserved.value().title_id);
  }
  CHECK(titles.front() == "FAKE78002" && titles[1] == "FAKE78005" && titles.back() == "FAKE78015");
  CHECK(std::set<std::string>(titles.begin(), titles.end()).size() == 12);
  CHECK(repository.Snapshot().size() == 13);

  // Removing a game frees its title for the next one, and only once it is gone.
  auto removed = *repository.Find("game-3");
  CHECK(removed.title_id == "FAKE78007");
  removed.state = PreparationState::kRemoving;
  CHECK(repository.Update(removed).ok());
  auto next = Request("game-new");
  next.kit_title_variants = range;
  auto reserved = repository.Reserve(next);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78016");  // still reserved while removing
  CHECK(repository.EraseAfterUnregister("game-3").ok());
  auto again = Request("game-again");
  again.kit_title_variants = range;
  reserved = repository.Reserve(again);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78007");

  // After a restart every reservation still holds, registered or not.
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", availability);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find("game-again")->title_id == "FAKE78007");
  auto after = Request("after-restart");
  after.kit_title_variants = range;
  reserved = restarted.Reserve(after);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78017");
}

void MalformedAndUnsupportedFilesDoNotBecomeEmptyRepositories() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  const auto write = [&](const std::string& text) {
    return storage.WriteAtomic("runtime-installs.json",
                               z3cloudplay::Bytes(text.begin(), text.end()));
  };

  CHECK(write("{broken").ok());
  RuntimeInstallRepository malformed(storage, "runtime-installs.json", Free);
  CHECK(malformed.Load().error() == RepositoryError::kMalformed);
  CHECK(malformed.Reserve(Request()).error() == RepositoryError::kNotLoaded);

  CHECK(write(R"({"schema_version":2,"reservations":[],"installs":[]})").ok());
  RuntimeInstallRepository unsupported(storage, "runtime-installs.json", Free);
  CHECK(unsupported.Load().error() == RepositoryError::kUnsupportedSchema);
  CHECK(unsupported.Reserve(Request()).error() == RepositoryError::kNotLoaded);

  CHECK(write(R"({"schema_version":1,"reservations":[],"installs":[{"schema_version":1,"install_id":"x","title_id":"FAKE78000","app_id":"220"}]})").ok());
  RuntimeInstallRepository wrong_type(storage, "runtime-installs.json", Free);
  CHECK(wrong_type.Load().error() == RepositoryError::kMalformed);

  std::string oversized(300 * 1024, 'x');
  CHECK(write(oversized).ok());
  RuntimeInstallRepository bounded(storage, "runtime-installs.json", Free);
  CHECK(bounded.Load().error() == RepositoryError::kBounds);
}

void LongLaunchArgumentsRoundTripWithinTheRepositoryBound() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  auto request = Request("long-arguments");
  request.launch_arguments = std::string(4096, 'a');
  CHECK(repository.Reserve(request).ok());
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find(request.install_id)->launch_arguments == request.launch_arguments);
  request.install_id = "overlong-arguments";
  request.launch_arguments.push_back('a');
  CHECK(!restarted.Reserve(request).ok());
  CHECK(!restarted.Find(request.install_id));
}

void RecordLimitDoesNotPublishAnUnreadableRepository() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  for (unsigned index = 0; index < 128; ++index) {
    auto request = Request("install-" + std::to_string(index));
    request.kit_title_variants = {"FAKE" + std::to_string(78000 + index)};
    CHECK(repository.Reserve(request).ok());
  }
  auto extra = Request("install-128");
  extra.kit_title_variants = {"FAKE78128"};
  CHECK(repository.Reserve(extra).error() == RepositoryError::kBounds);
  CHECK(repository.Reserve(Request("install-0")).ok());
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Snapshot().size() == 128);
}

void DescriptorIdentifiersMatchTheNativeGrammar() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  const std::vector<std::string> invalid = {
      "bad+id", "bad.id", "Bad", "_bad", "-bad", std::string(65, 'a')};
  for (const auto& value : invalid) {
    for (unsigned field = 0; field < 5; ++field) {
      auto request = Request();
      if (field == 0) request.install_id = value;
      if (field == 1) request.default_profile_id = value;
      if (field == 2) request.runtime_bundle_id = value;
      if (field == 3) request.recipe_id = value;
      if (field == 4) request.recipe_version = value;
      CHECK(repository.Reserve(request).error() == RepositoryError::kInvalidRecord);
      CHECK(repository.Snapshot().empty());
    }
  }
  auto valid = Request(std::string(64, 'a'));
  valid.default_profile_id = "0-game_profile";
  CHECK(repository.Reserve(valid).ok());
}

void LaunchPathsUseTheSharedSteamPathContract() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  auto request = Request();
  request.launch_path = ".\\bin//./Game.exe";
  request.launch_working_directory = ".\\bin//.";
  auto reserved = repository.Reserve(request);
  CHECK(reserved.ok());
  CHECK(reserved.value().launch_path == "bin/Game.exe");
  CHECK(reserved.value().launch_working_directory == "bin");
  auto changed = reserved.value();
  changed.launch_working_directory = "../../outside";
  CHECK(repository.Update(changed).error() == RepositoryError::kInvalidRecord);
  for (const auto& path : {"../../outside", "C:\\outside", "/outside", ".gn-download/x"}) {
    request.install_id = "invalid-path";
    request.launch_working_directory = path;
    CHECK(repository.Reserve(request).error() == RepositoryError::kInvalidRecord);
  }
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find("install-hl2")->launch_path == "bin/Game.exe");
}

void JournalSyncFailureRequiresReloadBeforeFurtherEffects() {
  for (const auto fault : {SyncFault::kFile, SyncFault::kDirectory}) {
    TemporaryDirectory directory;
    z3cloudplay::Storage storage(directory.path);
    RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
    CHECK(repository.Load().ok());
    sync_fault = fault;
    CHECK(repository.Reserve(Request()).error() == RepositoryError::kIo);
    CHECK(!repository.Find("install-hl2"));
    CHECK(repository.Save().error() == RepositoryError::kNotLoaded);
    sync_fault = SyncFault::kNone;
    CHECK(repository.Load().ok());
    // Rename may already have published the new record before directory sync.
    CHECK(bool(repository.Find("install-hl2")) == (fault == SyncFault::kDirectory));
    CHECK(repository.Reserve(Request()).ok());
    struct stat info{};
    CHECK(lstat((directory.path + "/runtime-installs.json").c_str(), &info) == 0);
    CHECK(S_ISREG(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 07777) == 0600);
  }
}

void JournalUpdateSyncFailureReconcilesPublishedState() {
  for (const auto fault : {SyncFault::kFile, SyncFault::kDirectory}) {
    TemporaryDirectory directory;
    z3cloudplay::Storage storage(directory.path);
    RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
    CHECK(repository.Load().ok());
    auto reserved = repository.Reserve(Request());
    CHECK(reserved.ok());
    auto changed = reserved.value();
    ++changed.settings_revision;
    sync_fault = fault;
    CHECK(repository.Update(changed).error() == RepositoryError::kIo);
    CHECK(repository.Load().error() == RepositoryError::kIo);
    CHECK(repository.Save().error() == RepositoryError::kNotLoaded);
    sync_fault = SyncFault::kNone;
    CHECK(repository.Load().ok());
    CHECK(repository.Find(changed.install_id)->settings_revision ==
          (fault == SyncFault::kDirectory ? changed.settings_revision : reserved.value().settings_revision));
  }
}

void RegistryPublicationRejectsSymlinkDestination() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  std::ofstream(directory.path + "/outside") << "sentinel";
  CHECK(symlink("outside", (directory.path + "/runtime-installs.json").c_str()) == 0);
  CHECK(repository.Reserve(Request()).error() == RepositoryError::kIo);
  CHECK(repository.Save().error() == RepositoryError::kNotLoaded);
  std::ifstream sentinel(directory.path + "/outside");
  std::string text; sentinel >> text;
  CHECK(text == "sentinel");
  CHECK(symlink(directory.path.c_str(), (directory.path + "/alias").c_str()) == 0);
  z3cloudplay::Storage alias(directory.path + "/alias");
  RuntimeInstallRepository linked_root(alias, "runtime-installs.json", Free);
  CHECK(linked_root.Load().error() == RepositoryError::kIo);
}

void RegistryRejectsSymlinksAndDoesNotTouchOldTemporaryFiles() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  std::ofstream(directory.path + "/outside") << "sentinel";
  CHECK(symlink("outside", (directory.path + "/runtime-installs.json.tmp").c_str()) == 0);
  CHECK(repository.Reserve(Request()).ok());
  std::ifstream sentinel(directory.path + "/outside");
  std::string text; sentinel >> text;
  CHECK(text == "sentinel");
  CHECK(unlink((directory.path + "/runtime-installs.json").c_str()) == 0);
  CHECK(symlink("outside", (directory.path + "/runtime-installs.json").c_str()) == 0);
  CHECK(repository.Load().error() == RepositoryError::kIo);
}

}  // namespace

int main() {
  JournalSyncFailureRequiresReloadBeforeFurtherEffects();
  JournalUpdateSyncFailureReconcilesPublishedState();
  RegistryRejectsSymlinksAndDoesNotTouchOldTemporaryFiles();
  RegistryPublicationRejectsSymlinkDestination();
  ReservationSurvivesRestartAndReinstallReusesIdentity();
  NativeInventoryAndSourceCollisionsAreNeverAllocated();
  AnyTitleRangeAllocatesBeyondTenAndReusesRemovedTitles();
  LegacyDownloadAdoptsWithoutCopyAndRemainsDownloaded();
  StateArtifactsFailureAndJournalRoundTrip();
  MalformedAndUnsupportedFilesDoNotBecomeEmptyRepositories();
  LongLaunchArgumentsRoundTripWithinTheRepositoryBound();
  DescriptorIdentifiersMatchTheNativeGrammar();
  RecordLimitDoesNotPublishAnUnreadableRepository();
  LaunchPathsUseTheSharedSteamPathContract();
  std::puts("runtime_game_install_test: all assertions passed");
  return EXIT_SUCCESS;
}
