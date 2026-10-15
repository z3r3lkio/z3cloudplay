#include "runtime/install_service.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace z3cloudplay::runtime;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #condition);                                                \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

namespace {

RuntimeInstall Downloaded(std::string id = "install-hl2") {
  RuntimeInstall r;
  r.install_id = std::move(id);
  r.title_id = "FAKE78001";
  r.app_id = 220;
  r.data_root = "/data/z3cloudplay/games/" + r.install_id;
  r.default_profile_id = "game";
  r.runtime_bundle_id = "prospero-win-e16ec8f";
  r.recipe_id = "steam-220-hl2-v1";
  r.recipe_version = "1";
  r.launch_entry_id = "0";
  r.launch_path = "hl2.exe";
  r.install_path = "/data/homebrew/FAKE77031/games/220";
  r.volume_id = "internal";
  return r;
}

struct FakeStore final : InstallRecordStore {
  std::unordered_map<std::string, RuntimeInstall> records;
  int updates = 0;
  std::function<bool(const RuntimeInstall &)> reject;
  std::optional<RuntimeInstall> Find(const std::string &id) const override {
    const auto it = records.find(id);
    return it == records.end() ? std::nullopt
                               : std::optional<RuntimeInstall>(it->second);
  }
  std::vector<RuntimeInstall> Snapshot() const override {
    std::vector<RuntimeInstall> out;
    for (const auto &entry : records)
      out.push_back(entry.second);
    return out;
  }
  RecordUpdateResult Update(const RuntimeInstall &record) override {
    ++updates;
    if (reject && reject(record))
      return {false, "injected-write", "injected persistence failure"};
    records[record.install_id] = record;
    return {true, {}, {}};
  }
};

struct FakePackage final : PackageInstaller {
  FakeStore *store = nullptr;
  PackageStatus status;
  std::string fail_step;
  int stage_calls = 0, copy_calls = 0, validate_calls = 0, profile_calls = 0;
  std::vector<std::string> observed_journals;
  std::function<void(const RuntimeInstall&)> profile_commit;
  PackageStatus Inspect(const RuntimeInstall &) override { return status; }
  PackageActionResult Action(const RuntimeInstall &record, const char *expected,
                             int &calls,
                             const std::function<void()> &complete) {
    ++calls;
    const auto current = store->Find(record.install_id);
    observed_journals.push_back(
        current && current->journal ? current->journal->step : "missing");
    if (fail_step == expected)
      return {PackageActionState::kFailed, expected, "injected failure"};
    complete();
    return {PackageActionState::kComplete, {}, {}};
  }
  PackageActionResult StageTitle(const RuntimeInstall &r) override {
    return Action(r, "stage-title", stage_calls, [&] {
      status.title_staged = true;
      status.manifest_matches = true;
    });
  }
  PackageActionResult CopyTemplate(const RuntimeInstall &r) override {
    return Action(r, "copy-template", copy_calls, [&] {
      status.template_owned = true;
      status.mutable_bottle_present = true;
      status.mutable_bottle_owned = true;
    });
  }
  PackageActionResult ValidateArtifacts(const RuntimeInstall &r) override {
    return Action(r, "validate-artifacts", validate_calls,
                  [&] { status.artifacts_valid = true; });
  }
  PackageActionResult PublishProfile(const RuntimeInstall &r) override {
    return Action(r, "publish-profile", profile_calls, [&] {
      status.profile_published = true;
      status.profile_settings_revision = r.settings_revision;
      if (profile_commit) profile_commit(r);
    });
  }
};

struct FakeNative final : NativeCoordinator {
  NativeActivity activity = NativeActivity::kStopped;
  NativeStatus status;
  RegistrationResult registration{RegistrationResultState::kScanQueued, {}, {}};
  int register_calls = 0;
  FakeStore *store = nullptr;
  NativeActivity Activity(const std::string &) override { return activity; }
  NativeStatus Query(const RuntimeInstall &) override { return status; }
  RegistrationResult Register(const RuntimeInstall &record) override {
    ++register_calls;
    const auto current = store->Find(record.install_id);
    CHECK(current && current->journal && current->journal->step == "register");
    if (registration.state == RegistrationResultState::kAccepted ||
        registration.state == RegistrationResultState::kScanQueued) {
      status.available = true;
      status.scan_pending = true;
    }
    return registration;
  }
};

void MakePackageCoherent(FakePackage &package, std::uint64_t revision = 0) {
  package.status.known = true;
  package.status.title_staged = true;
  package.status.manifest_matches = true;
  package.status.template_owned = true;
  package.status.mutable_bottle_present = true;
  package.status.mutable_bottle_owned = true;
  package.status.artifacts_valid = true;
  package.status.profile_published = true;
  package.status.profile_settings_revision = revision;
}

struct Fixture {
  FakeStore store;
  FakePackage package;
  FakeNative native;
  bool canceled = false;
  bool compatible = true;
  Fixture() {
    store.records.emplace("install-hl2", Downloaded());
    package.store = &store;
    package.status.known = true;
    native.store = &store;
    native.status.available = true;
    native.status.source_available = true;
    native.status.owned_manifest_matches = true;
  }
  RuntimeInstallService Service() {
    return RuntimeInstallService(
        store, package, native,
        [&](const RuntimeInstall &) {
          return CompatibilityResult{
              compatible, compatible ? "" : "compatible recipe is unavailable"};
        },
        [&] { return canceled; });
  }
};

void DownloadedIsNotReadyAndUnknownRecipeRemainsDownloaded() {
  Fixture f;
  f.compatible = false;
  auto service = f.Service();
  CHECK(service.Prepare("install-hl2").error() ==
        InstallServiceError::kIncompatible);
  const auto record = f.store.Find("install-hl2");
  CHECK(record && record->state == PreparationState::kDownloaded &&
        !record->journal);
  CHECK(record->last_failure &&
        record->last_failure->code == "recipe-unavailable");
  CHECK(f.package.stage_calls == 0 && f.native.register_calls == 0);
}

void EachSideEffectSeesItsDurableJournalAndScanQueuedIsPending() {
  Fixture f;
  auto service = f.Service();
  CHECK(service.Prepare("install-hl2").ok());
  CHECK(f.store.Find("install-hl2")->state == PreparationState::kPreparing);
  CHECK(service.Step("install-hl2").ok());
  CHECK(service.Step("install-hl2").ok());
  CHECK(service.Step("install-hl2").ok());
  CHECK(service.Step("install-hl2").ok());
  CHECK(service.Step("install-hl2").ok());
  CHECK((f.package.observed_journals ==
         std::vector<std::string>{"stage-title", "copy-template",
                                  "validate-artifacts", "publish-profile"}));
  const auto pending = f.store.Find("install-hl2");
  CHECK(pending && pending->state == PreparationState::kRegistering &&
        pending->journal && pending->journal->step == "await-native");
  CHECK(f.native.register_calls == 1);
  CHECK(service.Step("install-hl2").ok());
  CHECK(f.store.Find("install-hl2")->state == PreparationState::kRegistering);

  f.native.status = {true, true, true, true, false, false};
  CHECK(service.Step("install-hl2").ok());
  CHECK(f.store.Find("install-hl2")->state == PreparationState::kReady);
}

void FailuresAndCancellationPreserveDownloadAndJournal() {
  const std::vector<std::string> steps = {
      "stage-title", "copy-template", "validate-artifacts", "publish-profile"};
  for (const auto &failed_step : steps) {
    Fixture f;
    f.package.fail_step = failed_step;
    auto service = f.Service();
    CHECK(service.Prepare("install-hl2").ok());
    for (int i = 0; i < 4; ++i) {
      service.Step("install-hl2");
      if (f.store.Find("install-hl2")->state == PreparationState::kFailed)
        break;
    }
    const auto failed = f.store.Find("install-hl2");
    CHECK(failed && failed->state == PreparationState::kFailed &&
          failed->journal && failed->journal->step == failed_step);
    CHECK(failed->install_path == "/data/homebrew/FAKE77031/games/220");
    CHECK(failed->title_id == "FAKE78001");
  }

  Fixture canceled;
  auto service = canceled.Service();
  CHECK(service.Prepare("install-hl2").ok());
  canceled.canceled = true;
  CHECK(service.Step("install-hl2").error() == InstallServiceError::kCanceled);
  CHECK(canceled.package.stage_calls == 0);
  CHECK(canceled.store.Find("install-hl2")->journal->step == "stage-title");
}

void StaleHookRetainsJournalAndNeverRestartsAnything() {
  Fixture f;
  MakePackageCoherent(f.package);
  f.native.registration = {RegistrationResultState::kStaleHook, "stale-hook",
                           "registration hook is stale; restart it manually"};
  auto record = f.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "query-native"};
  f.store.records[record.install_id] = record;
  auto service = f.Service();
  CHECK(service.Step("install-hl2").error() ==
        InstallServiceError::kNativeUnavailable);
  const auto failed = f.store.Find("install-hl2");
  CHECK(failed && failed->state == PreparationState::kFailed &&
        failed->journal && failed->journal->step == "register");
  CHECK(failed->last_failure && failed->last_failure->code == "stale-hook");
  CHECK(f.native.register_calls == 1);
}

void RestartReusesIdentityAndPreservesExistingBottle() {
  Fixture f;
  auto record = f.store.records["install-hl2"];
  record.state = PreparationState::kPreparing;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "copy-template"};
  f.store.records[record.install_id] = record;
  f.package.status.known = true;
  f.package.status.title_staged = true;
  f.package.status.manifest_matches = true;
  f.package.status.template_owned = true;
  f.package.status.mutable_bottle_present = true;
  f.package.status.mutable_bottle_owned = true;

  auto restarted = f.Service();
  CHECK(restarted.Step("install-hl2").ok());
  CHECK(f.package.copy_calls == 0);
  CHECK(f.store.Find("install-hl2")->journal->step == "validate-artifacts");
  CHECK(f.store.Find("install-hl2")->title_id == "FAKE78001");

  f.native.status = {true, true, true, true, false, false};
  MakePackageCoherent(f.package);
  record = *f.store.Find("install-hl2");
  record.state = PreparationState::kRegistering;
  record.journal->step = "await-native";
  f.store.records[record.install_id] = record;
  auto reopened = f.Service();
  CHECK(reopened.Step("install-hl2").ok());
  CHECK(f.native.register_calls == 0);
  CHECK(f.store.Find("install-hl2")->state == PreparationState::kReady);
}

void ActivityUnknownBlocksMutationAndReadyWriteFailureIsNotFalseReady() {
  Fixture blocked;
  blocked.native.activity = NativeActivity::kUnknown;
  auto blocked_service = blocked.Service();
  CHECK(blocked_service.Prepare("install-hl2").ok());
  CHECK(blocked_service.Step("install-hl2").error() ==
        InstallServiceError::kActivityUnknown);
  CHECK(blocked.package.stage_calls == 0);

  Fixture ready;
  MakePackageCoherent(ready.package);
  auto record = ready.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "await-native"};
  ready.store.records[record.install_id] = record;
  ready.native.status = {true, true, true, true, false, false};
  ready.store.reject = [](const RuntimeInstall &candidate) {
    return candidate.state == PreparationState::kReady;
  };
  auto service = ready.Service();
  CHECK(service.Step("install-hl2").error() ==
        InstallServiceError::kPersistence);
  CHECK(ready.store.Find("install-hl2")->state ==
        PreparationState::kRegistering);
  CHECK(ready.store.Find("install-hl2")->journal->step == "await-native");
  ready.store.reject = {};
  CHECK(service.Step("install-hl2").ok());
  CHECK(ready.store.Find("install-hl2")->state == PreparationState::kReady);
}

void ForeignBottleAndStaleProfileNeverBecomeReady() {
  Fixture foreign;
  auto record = foreign.store.records["install-hl2"];
  record.state = PreparationState::kPreparing;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "copy-template"};
  foreign.store.records[record.install_id] = record;
  foreign.package.status.known = true;
  foreign.package.status.title_staged = true;
  foreign.package.status.manifest_matches = true;
  foreign.package.status.mutable_bottle_present = true;
  auto service = foreign.Service();
  CHECK(service.Step("install-hl2").error() ==
        InstallServiceError::kOwnershipConflict);
  CHECK(foreign.package.copy_calls == 0);
  CHECK(foreign.store.Find("install-hl2")->journal->step == "copy-template");

  Fixture stale;
  MakePackageCoherent(stale.package, 1);
  record = stale.store.records["install-hl2"];
  record.settings_revision = 2;
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "await-native"};
  stale.store.records[record.install_id] = record;
  stale.native.status = {true, true, true, true, false, false};
  auto stale_service = stale.Service();
  CHECK(stale_service.Step("install-hl2").error() ==
        InstallServiceError::kOwnershipConflict);
  CHECK(stale.store.Find("install-hl2")->state == PreparationState::kFailed);
}

void PersistenceBeforeEffectAndRegistrationFailureAreRecoverable() {
  Fixture persistence;
  persistence.store.reject = [](const RuntimeInstall &candidate) {
    return candidate.journal && candidate.journal->step == "stage-title";
  };
  auto service = persistence.Service();
  CHECK(service.Prepare("install-hl2").error() ==
        InstallServiceError::kPersistence);
  CHECK(persistence.package.stage_calls == 0);
  CHECK(persistence.store.Find("install-hl2")->state ==
        PreparationState::kDownloaded);

  Fixture registration;
  MakePackageCoherent(registration.package);
  auto record = registration.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "query-native"};
  registration.store.records[record.install_id] = record;
  registration.native.registration = {RegistrationResultState::kFailed,
                                      "register-busy",
                                      "native registration is busy"};
  auto registration_service = registration.Service();
  CHECK(registration_service.Step("install-hl2").error() ==
        InstallServiceError::kNativeUnavailable);
  const auto failed = registration.store.Find("install-hl2");
  CHECK(failed && failed->state == PreparationState::kFailed &&
        failed->journal && failed->journal->step == "register" &&
        failed->title_id == "FAKE78001");
  CHECK(registration.native.register_calls == 1);
  CHECK(registration_service.Retry("install-hl2").ok());
  CHECK(registration.store.Find("install-hl2")->journal->step ==
        "query-native");
}

void RestartAfterCompletedEffectsDoesNotRepeatThem() {
  struct Boundary {
    const char *current;
    const char *next;
  };
  const Boundary boundaries[] = {{"stage-title", "copy-template"},
                                 {"copy-template", "validate-artifacts"},
                                 {"validate-artifacts", "publish-profile"},
                                 {"publish-profile", "query-native"}};
  for (const auto &boundary : boundaries) {
    Fixture f;
    auto record = f.store.records["install-hl2"];
    record.state = PreparationState::kPreparing;
    record.journal =
        PreparationJournal{"prepare-install-hl2", "prepare", boundary.current};
    f.store.records[record.install_id] = record;
    const std::string current = boundary.current;
    if (current != "stage-title") {
      f.package.status.title_staged = true;
      f.package.status.manifest_matches = true;
    }
    if (current == "validate-artifacts" || current == "publish-profile") {
      f.package.status.template_owned = true;
      f.package.status.mutable_bottle_present = true;
      f.package.status.mutable_bottle_owned = true;
    }
    if (current == "publish-profile")
      f.package.status.artifacts_valid = true;
    f.store.reject = [&](const RuntimeInstall &candidate) {
      return candidate.journal && candidate.journal->step == boundary.next;
    };
    auto service = f.Service();
    CHECK(service.Step("install-hl2").error() ==
          InstallServiceError::kPersistence);
    const int calls = f.package.stage_calls + f.package.copy_calls +
                      f.package.validate_calls + f.package.profile_calls;
    CHECK(calls == 1);
    CHECK(f.store.Find("install-hl2")->journal->step == boundary.current);
    f.store.reject = {};
    auto restarted = f.Service();
    CHECK(restarted.Step("install-hl2").ok());
    CHECK(f.package.stage_calls + f.package.copy_calls +
              f.package.validate_calls + f.package.profile_calls ==
          calls);
    CHECK(f.store.Find("install-hl2")->journal->step == boundary.next);
  }

  Fixture registration;
  MakePackageCoherent(registration.package);
  auto record = registration.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "query-native"};
  registration.store.records[record.install_id] = record;
  registration.store.reject = [](const RuntimeInstall &candidate) {
    return candidate.journal && candidate.journal->step == "await-native";
  };
  auto service = registration.Service();
  CHECK(service.Step("install-hl2").error() ==
        InstallServiceError::kPersistence);
  CHECK(registration.native.register_calls == 1);
  CHECK(registration.store.Find("install-hl2")->journal->step == "register");
  registration.store.reject = {};
  auto restarted = registration.Service();
  CHECK(restarted.Step("install-hl2").ok());
  CHECK(registration.native.register_calls == 1);
  CHECK(registration.store.Find("install-hl2")->journal->step ==
        "await-native");
}

void OwnedSourceRegistersAndIncoherentNativeStatesDoNot() {
  Fixture owned_source;
  MakePackageCoherent(owned_source.package);
  auto record = owned_source.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "query-native"};
  owned_source.store.records[record.install_id] = record;
  owned_source.native.status = {true, false, true, true, false, false};
  auto service = owned_source.Service();
  CHECK(service.Step("install-hl2").ok());
  CHECK(owned_source.native.register_calls == 1);
  CHECK(owned_source.store.Find("install-hl2")->journal->step ==
        "await-native");

  Fixture absent;
  MakePackageCoherent(absent.package);
  record = absent.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "query-native"};
  absent.store.records[record.install_id] = record;
  absent.native.status = {true, false, false, false, false, false};
  auto absent_service = absent.Service();
  CHECK(absent_service.Step("install-hl2").error() ==
        InstallServiceError::kNativeIncoherent);
  CHECK(absent.native.register_calls == 0);
  CHECK(absent.store.Find("install-hl2")->journal->step == "query-native");

  Fixture installed_without_source;
  MakePackageCoherent(installed_without_source.package);
  record = installed_without_source.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "await-native"};
  installed_without_source.store.records[record.install_id] = record;
  installed_without_source.native.status = {true, true,  false,
                                            true, false, false};
  auto incoherent_service = installed_without_source.Service();
  CHECK(incoherent_service.Step("install-hl2").error() ==
        InstallServiceError::kNativeIncoherent);
  CHECK(installed_without_source.native.register_calls == 0);
  CHECK(installed_without_source.store.Find("install-hl2")->journal->step ==
        "await-native");

  Fixture scanning;
  MakePackageCoherent(scanning.package);
  record = scanning.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "await-native"};
  scanning.store.records[record.install_id] = record;
  scanning.native.status = {true, false, false, false, false, true};
  auto scanning_service = scanning.Service();
  CHECK(scanning_service.Step("install-hl2").ok());
  CHECK(scanning.native.register_calls == 0);
  CHECK(scanning.store.Find("install-hl2")->state ==
        PreparationState::kRegistering);
}

void ExplicitRetryRecoversUncertainAwaitWithoutAutomaticResubmit() {
  Fixture f;
  MakePackageCoherent(f.package);
  auto record = f.store.records["install-hl2"];
  record.state = PreparationState::kRegistering;
  record.journal =
      PreparationJournal{"prepare-install-hl2", "prepare", "await-native"};
  f.store.records[record.install_id] = record;
  f.native.status = {true, false, true, true, false, false};

  auto service = f.Service();
  CHECK(service.Step("install-hl2").error() ==
        InstallServiceError::kRegistrationPending);
  CHECK(f.native.register_calls == 0);
  CHECK(f.store.Find("install-hl2")->state == PreparationState::kFailed);
  CHECK(f.store.Find("install-hl2")->journal->step == "await-native");

  auto reopened = f.Service();
  CHECK(reopened.Step("install-hl2").error() ==
        InstallServiceError::kInvalidState);
  CHECK(f.native.register_calls == 0);
  CHECK(reopened.Retry("install-hl2").ok());
  CHECK(f.store.Find("install-hl2")->journal->step == "query-native");
  CHECK(reopened.Step("install-hl2").ok());
  CHECK(f.native.register_calls == 1);
}

void PublicationDriverOwnsLatestRecordAndRecoveryJournal() {
  for (const bool pending : {false, true}) {
    Fixture f; MakePackageCoherent(f.package);
    auto& initial = f.store.records["install-hl2"];
    initial.state = PreparationState::kPreparing;
    initial.journal = PreparationJournal{"prepare-install-hl2", "prepare", "publish-profile"};
    unsigned calls = 0;
    InstallPublicationDriver publication = [&](const RuntimeInstall& record) {
      ++calls;
      auto latest = *f.store.Find(record.install_id);
      latest.journal = PreparationJournal{"11111111-1111-4111-8111-111111111111", "settings-publication", "await-native"};
      latest.state = PreparationState::kRegistering;
      if (!pending || calls > 1) { latest.state = PreparationState::kReady; latest.journal.reset(); }
      CHECK(f.store.Update(latest).ok);
      return latest.state == PreparationState::kReady ? PackageActionResult{PackageActionState::kComplete, {}, {}} :
          PackageActionResult{PackageActionState::kFailed, "publication-registration-pending", "registration is pending"};
    };
    RuntimeInstallService service(f.store, f.package, f.native, {}, {}, publication);
    const auto result = service.Step("install-hl2");
    CHECK(calls == 1 && f.package.profile_calls == 0 && f.native.register_calls == 0);
    CHECK(result.ok() == !pending);
    if (pending) CHECK(result.error() == InstallServiceError::kRegistrationPending);
    auto observed = f.store.Find("install-hl2"); CHECK(observed && !observed->last_failure);
    if (pending) {
      CHECK(observed->state == PreparationState::kRegistering && observed->journal->operation == "settings-publication");
      CHECK(observed->journal->step == "await-native");
      RuntimeInstallService restarted(f.store, f.package, f.native, {}, {}, publication);
      CHECK(restarted.Step("install-hl2").ok() && calls == 2);
    }
    CHECK(f.store.Find("install-hl2")->state == PreparationState::kReady && !f.store.Find("install-hl2")->journal);
  }
}
void ManagedJournalsNeverFallThroughToLegacyRegistrationOrRetry() {
  for (const auto* operation : {"settings-control", "settings-publication"}) {
    Fixture f; MakePackageCoherent(f.package);
    auto& initial = f.store.records["install-hl2"];
    initial.state = PreparationState::kRegistering;
    initial.journal = PreparationJournal{"11111111-1111-4111-8111-111111111111", operation, "await-native"};
    const auto before = f.store.updates;
    auto legacy = f.Service();
    CHECK(!legacy.Step("install-hl2").ok());
    CHECK(f.store.updates == before && f.native.register_calls == 0 && f.package.profile_calls == 0);
    initial.state = PreparationState::kFailed;
    CHECK(!legacy.Retry("install-hl2").ok()); CHECK(f.store.updates == before);
    // Retry must preserve a managed registration arm; it cannot rearm native
    // submission by changing await-native to the legacy query-native step.
    InstallPublicationDriver publication = [](const RuntimeInstall&) { return PackageActionResult{PackageActionState::kFailed, "pending", "pending"}; };
    RuntimeInstallService service(f.store, f.package, f.native, {}, {}, publication);
    CHECK(service.Retry("install-hl2").ok());
    CHECK(f.store.Find("install-hl2")->journal->operation == operation && f.store.Find("install-hl2")->journal->step == "await-native");
    CHECK(!service.Step("install-hl2").ok() && f.native.register_calls == 0);
  }
}
void PackageCallbackCannotOverwriteNewerOwnerJournal() {
  for (const bool ready : {false, true}) {
    Fixture f; MakePackageCoherent(f.package); f.package.status.profile_published = false;
    auto& initial = f.store.records["install-hl2"]; initial.state = PreparationState::kPreparing;
    initial.journal = PreparationJournal{"prepare-install-hl2", "prepare", "publish-profile"};
    f.package.profile_commit = [&](const RuntimeInstall& record) {
      auto latest = record; latest.state = ready ? PreparationState::kReady : PreparationState::kRegistering;
      latest.journal = PreparationJournal{"11111111-1111-4111-8111-111111111111", "settings-publication", "await-native"};
      if (ready) latest.journal.reset();
      CHECK(f.store.Update(latest).ok);
    };
    auto service = f.Service(); const auto result = service.Step("install-hl2");
    CHECK(f.store.updates == 1 && f.native.register_calls == 0 && result.ok() == ready);
    auto observed = f.store.Find("install-hl2"); CHECK(!observed->last_failure);
    if (!ready) CHECK(observed->state == PreparationState::kRegistering && observed->journal->operation == "settings-publication" && observed->journal->step == "await-native");
    else CHECK(observed->state == PreparationState::kReady && !observed->journal);
  }
}

void ManagedDriverCannotClaimCompleteBeforeReady() {
  Fixture f; MakePackageCoherent(f.package);
  auto& initial = f.store.records["install-hl2"];
  initial.state = PreparationState::kPreparing;
  initial.journal = PreparationJournal{"11111111-1111-4111-8111-111111111111", "settings-control", "control-ready"};
  unsigned calls = 0;
  RuntimeInstallService service(f.store, f.package, f.native, {}, {}, [&](const RuntimeInstall&) {
    ++calls; return PackageActionResult{PackageActionState::kComplete, {}, {}};
  });
  CHECK(!service.Step("install-hl2").ok() && calls == 1 && f.store.updates == 0);
  f.native.activity = NativeActivity::kActive;
  CHECK(service.Step("install-hl2").error() == InstallServiceError::kActivityActive && calls == 1 && f.store.updates == 0);
  f.native.activity = NativeActivity::kUnknown;
  CHECK(service.Step("install-hl2").error() == InstallServiceError::kActivityUnknown && calls == 1 && f.store.updates == 0);
  f.native.activity = NativeActivity::kStopped;
  RuntimeInstallService canceled(f.store, f.package, f.native, {}, [] { return true; }, [&](const RuntimeInstall&) {
    ++calls; return PackageActionResult{PackageActionState::kComplete, {}, {}};
  });
  CHECK(canceled.Step("install-hl2").error() == InstallServiceError::kCanceled && calls == 1 && f.store.updates == 0);
}

} // namespace

int main() {
  PackageCallbackCannotOverwriteNewerOwnerJournal();
  PublicationDriverOwnsLatestRecordAndRecoveryJournal();
  ManagedJournalsNeverFallThroughToLegacyRegistrationOrRetry();
  ManagedDriverCannotClaimCompleteBeforeReady();
  DownloadedIsNotReadyAndUnknownRecipeRemainsDownloaded();
  EachSideEffectSeesItsDurableJournalAndScanQueuedIsPending();
  FailuresAndCancellationPreserveDownloadAndJournal();
  StaleHookRetainsJournalAndNeverRestartsAnything();
  RestartReusesIdentityAndPreservesExistingBottle();
  ActivityUnknownBlocksMutationAndReadyWriteFailureIsNotFalseReady();
  ForeignBottleAndStaleProfileNeverBecomeReady();
  PersistenceBeforeEffectAndRegistrationFailureAreRecoverable();
  RestartAfterCompletedEffectsDoesNotRepeatThem();
  OwnedSourceRegistersAndIncoherentNativeStatesDoNot();
  ExplicitRetryRecoversUncertainAwaitWithoutAutomaticResubmit();
  std::puts("runtime_install_service_test: all assertions passed");
  return EXIT_SUCCESS;
}
