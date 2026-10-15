#include "runtime/session_reconciler.h"
#include "steam/backend/json.h"
extern "C" {
#include "pw_title_session.h"
}
#include <filesystem>
#include <fstream>
#include <cstring>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace z3cloudplay::runtime;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #condition);                                                 \
      std::exit(1);                                                             \
    }                                                                           \
  } while (false)

namespace {

constexpr const char *kSession = "123e4567-e89b-42d3-a456-426614174000";
constexpr const char *kOtherSession = "223e4567-e89b-42d3-a456-426614174000";

template <typename T, typename = void> struct HasIndependentAppId : std::false_type {};
template <typename T>
struct HasIndependentAppId<T, std::void_t<decltype(std::declval<T>().app_id)>>
    : std::true_type {};

// pw_title_session.c writes these identity fields in session v1. app_id is
// deliberately absent: the owned RuntimeInstall supplies it to CloudLocation.
static_assert(!HasIndependentAppId<WrapperSession>::value,
              "wrapper session v1 must not invent independent app_id evidence");


RuntimeInstall ReadyInstall() {
  RuntimeInstall record;
  record.install_id = "install-hl2";
  record.title_id = "FAKE78001";
  record.app_id = 220;
  record.data_root = "/data/z3cloudplay/games/install-hl2";
  record.runtime_bundle_id = "prospero-win-e16ec8f";
  record.settings_revision = 17;
  record.install_path = "/games/220";
  record.state = PreparationState::kReady;
  return record;
}

WrapperSession CleanSession(SessionOrigin origin = SessionOrigin::kZ3CloudPlay) {
  const auto record = ReadyInstall();
  WrapperSession session;
  session.session_id = kSession;
  session.origin = origin;
  session.install_id = record.install_id;
  session.title_id = record.title_id;
  session.runtime_bundle_id = record.runtime_bundle_id;
  session.settings_revision = record.settings_revision;
  session.phase = WrapperSessionPhase::kExited;
  session.exit_code = 0;
  return session;
}

struct Source final : ReconcileSessionSource {
  SessionEnumeration next = SessionEnumeration::Success({CleanSession()});
  int calls = 0;
  std::size_t last_limit = 0;
  SessionEnumeration Enumerate(const RuntimeInstall &, std::size_t limit) override {
    ++calls;
    last_limit = limit;
    return next;
  }
};

struct Journal final : ReconcileJournal {
  std::unordered_map<std::string, ReconcileEntry> entries;
  int loads = 0;
  int saves = 0;
  int fail_save_call = 0;

  ReconcileJournalLoad Load(const std::string &session_id) override {
    ++loads;
    const auto found = entries.find(session_id);
    return ReconcileJournalLoad::Success(
        found == entries.end() ? std::optional<ReconcileEntry>{}
                               : std::optional<ReconcileEntry>{found->second});
  }
  ReconcileJournalResult Save(const ReconcileEntry &entry) override {
    ++saves;
    if (fail_save_call != 0 && saves == fail_save_call)
      return ReconcileJournalResult::Failure("write-failed", "journal write failed");
    entries[entry.session.session_id] = entry;
    return ReconcileJournalResult::Success();
  }
};

struct Cloud final : ReconcileCloud {
  CloudOperationResult start_result = CloudOperationResult::Running(
      "operation-1", kSession, "account-a");
  CloudOperationResult poll_result = CloudOperationResult::Succeeded(
      "operation-1", kSession, "account-a");
  int starts = 0;
  int polls = 0;
  int cancels = 0;
  SessionCloudRequest request;
  std::string polled_operation;
  std::string canceled_operation;

  CloudOperationResult StartCheck(const SessionCloudRequest &value) override {
    ++starts;
    request = value;
    return start_result;
  }
  CloudOperationResult Poll(const std::string &operation_id,
                            const std::string &session_id,
                            const std::string &account_token) override {
    ++polls;
    polled_operation = operation_id + ":" + session_id + ":" + account_token;
    return poll_result;
  }
  void Cancel(const std::string &operation_id, const std::string &,
              const std::string &) override {
    ++cancels;
    canceled_operation = operation_id;
  }
};

const SessionReconcileStatus *Status(const ReconcileResult &result,
                                     const std::string &session_id) {
  for (const auto &status : result.sessions)
    if (status.session_id == session_id) return &status;
  return nullptr;
}

struct Fixture {
  Source source;
  Journal journal;
  Cloud cloud;
  NativeActivity activity = NativeActivity::kStopped;
  std::string account = "account-a";
  bool owned = true;
  std::vector<RuntimeInstall> records{ReadyInstall()};

  SessionReconcileDependencies Dependencies() {
    SessionReconcileDependencies dependencies;
    dependencies.activity = [&](const std::string &) { return activity; };
    dependencies.current_account_token = [&] { return account; };
    dependencies.owned = [&](const RuntimeInstall &) { return owned; };
    return dependencies;
  }
  SessionReconciler Reconciler() {
    return SessionReconciler(source, journal, cloud, Dependencies());
  }
};

void ActualWrapperRecordsAreAccepted() {
  namespace fs = std::filesystem;
  for (const char* argument : {"session=123e4567-e89b-42d3-a456-426614174000",
                               "session=123e4567-e89b-12d3-a456-426614174000",
                               static_cast<const char*>(nullptr)}) {
    char scratch[] = "/tmp/z3cloudplay-session-producer-XXXXXX";
    CHECK(mkdtemp(scratch));
    const auto root = fs::canonical(scratch);
    Fixture f;
    f.records.front().data_root = root.string();
    PwTitleLaunch launch{};
    launch.schema_version = 1;
    launch.app_id = f.records.front().app_id;
    launch.settings_revision = f.records.front().settings_revision;
    const auto copy = [](auto& output, const std::string& input) {
      CHECK(input.size() < sizeof(output));
      std::strcpy(output, input.c_str());
    };
    copy(launch.install_id, f.records.front().install_id);
    copy(launch.title_id, f.records.front().title_id);
    copy(launch.runtime_bundle_id, f.records.front().runtime_bundle_id);
    copy(launch.data_root, root.string());
    PwTitleSession produced{};
    CHECK(pw_title_session_start(&produced, &launch, argument, nullptr, 0) == 0);
    CHECK(pw_title_session_running(&produced) == 0);
    CHECK(pw_title_session_exited(&produced, 1, 0) == 0);
    std::ifstream file(produced.path);
    const std::string bytes{std::istreambuf_iterator<char>(file), {}};
    const auto document = z3cloudplay::steam::json::Parse(bytes);
    CHECK(document && document->is_object());
    CHECK(!document->Find("app_id"));
    WrapperSession session;
    const auto text = [&](const char* key) {
      const auto* value = document->Find(key);
      CHECK(value && value->is_string());
      return value->text();
    };
    session.session_id = text("session_id");
    session.install_id = text("install_id");
    session.title_id = text("title_id");
    session.runtime_bundle_id = text("runtime_bundle_id");
    session.origin = text("origin") == "home" ? SessionOrigin::kHome : SessionOrigin::kZ3CloudPlay;
    CHECK(document->Find("schema_version")->AsUint64() == 1);
    session.settings_revision = *document->Find("settings_revision")->AsUint64();
    CHECK(text("phase") == "exited");
    session.phase = WrapperSessionPhase::kExited;
    CHECK(document->Find("exit_code")->AsUint64() == 0);
    session.exit_code = 0;
    f.source.next = SessionEnumeration::Success({session});
    f.cloud.start_result = CloudOperationResult::Succeeded("operation-1", session.session_id, "account-a");
    auto reconciler = f.Reconciler();
    const auto result = reconciler.Reconcile(f.records);
    CHECK(Status(result, session.session_id)->state == SessionReconcileState::kCompleted);
    CHECK(f.cloud.request.location.app_id == 220);
    CHECK(f.cloud.starts == 1);
    pw_title_session_close(&produced);
    file.close();
    fs::remove_all(root);
  }
}

void CleanExitUsesActualBottleAndCompletesOnlyAfterTerminalCloudResult() {
  Fixture f;
  auto reconciler = f.Reconciler();
  const auto started = reconciler.Reconcile(f.records);
  CHECK(Status(started, kSession)->state == SessionReconcileState::kCloudRunning);
  CHECK(f.cloud.starts == 1 && f.cloud.polls == 0);
  CHECK(f.cloud.request.account_token == "account-a");
  CHECK(f.cloud.request.location.app_id == 220);
  CHECK(f.cloud.request.location.game_root == "/games/220");
  CHECK(f.cloud.request.location.state_root ==
        "/data/z3cloudplay/games/install-hl2/cloud");
  CHECK(f.cloud.request.location.prefix_root ==
        "/data/z3cloudplay/games/install-hl2/library/prefixes/game");
  CHECK(f.journal.entries.at(kSession).phase == ReconcilePhase::kCloudRunning);

  const auto completed = reconciler.Reconcile(f.records);
  CHECK(Status(completed, kSession)->state == SessionReconcileState::kCompleted);
  CHECK(f.cloud.starts == 1 && f.cloud.polls == 1);
  CHECK(f.journal.entries.at(kSession).phase == ReconcilePhase::kCompleted);
}

void HomeOriginIsAcceptedAndBoundToCloudRequest() {
  Fixture f;
  f.source.next = SessionEnumeration::Success({CleanSession(SessionOrigin::kHome)});
  auto reconciler = f.Reconciler();
  CHECK(Status(reconciler.Reconcile(f.records), kSession)->state ==
        SessionReconcileState::kCloudRunning);
  CHECK(f.cloud.request.session.origin == SessionOrigin::kHome);
}

void InterruptedAndUncleanSessionsRequireRecoveryConfirmation() {
  const std::vector<WrapperSession> sessions = [] {
    std::vector<WrapperSession> values;
    auto failed = CleanSession();
    failed.phase = WrapperSessionPhase::kFailed;
    failed.exit_code.reset();
    values.push_back(failed);
    auto nonzero = CleanSession();
    nonzero.session_id = kOtherSession;
    nonzero.exit_code = 7;
    values.push_back(nonzero);
    return values;
  }();
  Fixture f;
  f.source.next = SessionEnumeration::Success(sessions);
  auto reconciler = f.Reconciler();
  const auto result = reconciler.Reconcile(f.records);
  CHECK(Status(result, kSession)->state ==
        SessionReconcileState::kNeedsRecoveryConfirmation);
  CHECK(Status(result, kOtherSession)->state ==
        SessionReconcileState::kNeedsRecoveryConfirmation);
  CHECK(f.cloud.starts == 0);
  CHECK(reconciler.ConfirmRecovery(kSession, "account-a").ok);
  const auto confirmed = reconciler.Reconcile(f.records);
  CHECK(Status(confirmed, kSession)->state == SessionReconcileState::kCloudRunning);
  CHECK(Status(confirmed, kOtherSession)->state ==
        SessionReconcileState::kNeedsRecoveryConfirmation);
  CHECK(f.cloud.starts == 1);
}

void StartingOrRunningAfterStopIsInterruptedAndMissingEvidenceStaysUnresolved() {
  for (const auto phase : {WrapperSessionPhase::kStarting,
                           WrapperSessionPhase::kRunning}) {
    Fixture f;
    auto session = CleanSession();
    session.phase = phase;
    session.exit_code.reset();
    f.source.next = SessionEnumeration::Success({session});
    auto reconciler = f.Reconciler();
    CHECK(Status(reconciler.Reconcile(f.records), kSession)->state ==
          SessionReconcileState::kNeedsRecoveryConfirmation);
    CHECK(f.cloud.starts == 0);
  }

  Fixture missing;
  missing.source.next = SessionEnumeration::Success({});
  auto reconciler = missing.Reconciler();
  const auto result = reconciler.Reconcile(missing.records);
  CHECK(result.unresolved_records == 1 && result.sessions.empty());
  CHECK(missing.journal.entries.empty() && missing.cloud.starts == 0);
}

void ActiveAndUnknownTitlesNeverDispatchOrPollAndCancelOutstandingWork() {
  for (const auto activity : {NativeActivity::kActive, NativeActivity::kUnknown}) {
    Fixture fresh;
    fresh.activity = activity;
    auto reconciler = fresh.Reconciler();
    CHECK(Status(reconciler.Reconcile(fresh.records), kSession)->state ==
          SessionReconcileState::kBlockedByActivity);
    CHECK(fresh.cloud.starts == 0);

    Fixture running;
    ReconcileEntry entry;
    entry.session = CleanSession();
    entry.account_token = "account-a";
    entry.phase = ReconcilePhase::kCloudRunning;
    entry.operation_id = "operation-1";
    running.journal.entries[kSession] = entry;
    running.activity = activity;
    auto resumed = running.Reconciler();
    CHECK(Status(resumed.Reconcile(running.records), kSession)->state ==
          SessionReconcileState::kBlockedByActivity);
    CHECK(running.cloud.polls == 0 && running.cloud.cancels == 1);
    CHECK(running.journal.entries.at(kSession).phase ==
          ReconcilePhase::kDispatchPending);
    CHECK(running.journal.entries.at(kSession).operation_id.empty());
    running.activity = NativeActivity::kStopped;
    CHECK(Status(resumed.Reconcile(running.records), kSession)->state ==
          SessionReconcileState::kCloudRunning);
    CHECK(running.cloud.starts == 1);
  }
}

void DuplicatesAndRestartedCompletedSessionsNeverQueueAgain() {
  Fixture f;
  f.source.next = SessionEnumeration::Success({CleanSession(), CleanSession()});
  f.cloud.start_result =
      CloudOperationResult::Succeeded("operation-1", kSession, "account-a");
  auto first = f.Reconciler();
  const auto completed = first.Reconcile(f.records);
  CHECK(Status(completed, kSession)->state == SessionReconcileState::kCompleted);
  CHECK(completed.sessions.size() == 1 && f.cloud.starts == 1);

  auto restarted = f.Reconciler();
  CHECK(Status(restarted.Reconcile(f.records), kSession)->state ==
        SessionReconcileState::kCompleted);
  CHECK(f.cloud.starts == 1 && f.cloud.polls == 0);
}

void IdentitySpoofsAndUnownedOrUnreadyRecordsAreRejected() {
  using Mutator = void (*)(WrapperSession &);
  const std::vector<Mutator> spoofs = {
      [](auto &s) { s.install_id = "other"; },
      [](auto &s) { s.title_id = "FAKE78002"; },
      [](auto &s) { s.runtime_bundle_id = "other"; },
      [](auto &s) { ++s.settings_revision; },
      [](auto &s) { s.session_id = "../../bad"; },
  };
  for (const auto mutate : spoofs) {
    Fixture f;
    auto session = CleanSession();
    mutate(session);
    f.source.next = SessionEnumeration::Success({session});
    auto reconciler = f.Reconciler();
    const auto result = reconciler.Reconcile(f.records);
    CHECK(result.rejected_sessions == 1 && f.cloud.starts == 0);
    CHECK(f.journal.entries.empty());
  }

  Fixture unowned;
  unowned.owned = false;
  auto no_owner = unowned.Reconciler();
  CHECK(no_owner.Reconcile(unowned.records).sessions.empty());
  CHECK(unowned.source.calls == 0);

  Fixture unready;
  unready.records[0].state = PreparationState::kPreparing;
  auto no_ready = unready.Reconciler();
  CHECK(no_ready.Reconcile(unready.records).sessions.empty());
  CHECK(unready.source.calls == 0);
}

void AccountChangesAndStaleCloudResultsAreCanceledAndNeverComplete() {
  Fixture switched;
  ReconcileEntry entry;
  entry.session = CleanSession();
  entry.account_token = "account-a";
  entry.phase = ReconcilePhase::kCloudRunning;
  entry.operation_id = "operation-1";
  switched.journal.entries[kSession] = entry;
  switched.account = "account-b";
  auto resumed = switched.Reconciler();
  CHECK(Status(resumed.Reconcile(switched.records), kSession)->state ==
        SessionReconcileState::kFailed);
  CHECK(switched.cloud.cancels == 1 && switched.cloud.polls == 0);
  CHECK(switched.journal.entries.at(kSession).account_token == "account-a");
  CHECK(switched.journal.entries.at(kSession).phase ==
        ReconcilePhase::kDispatchPending);
  CHECK(switched.journal.entries.at(kSession).operation_id.empty());
  switched.account = "account-a";
  CHECK(Status(resumed.Reconcile(switched.records), kSession)->state ==
        SessionReconcileState::kCloudRunning);
  CHECK(switched.cloud.starts == 1);

  Fixture stale;
  stale.cloud.start_result =
      CloudOperationResult::Running("operation-1", kOtherSession, "account-a");
  auto stale_reconciler = stale.Reconciler();
  CHECK(Status(stale_reconciler.Reconcile(stale.records), kSession)->state ==
        SessionReconcileState::kFailed);
  CHECK(stale.cloud.cancels == 1);
  CHECK(stale.journal.entries.at(kSession).phase == ReconcilePhase::kFailed);

  Fixture stale_poll;
  entry.phase = ReconcilePhase::kCloudRunning;
  entry.operation_id = "operation-1";
  stale_poll.journal.entries[kSession] = entry;
  stale_poll.cloud.poll_result =
      CloudOperationResult::Succeeded("operation-2", kSession, "account-a");
  auto stale_poll_reconciler = stale_poll.Reconciler();
  CHECK(Status(stale_poll_reconciler.Reconcile(stale_poll.records), kSession)
            ->state == SessionReconcileState::kFailed);
  CHECK(stale_poll.cloud.polls == 1 && stale_poll.cloud.cancels == 1);
  CHECK(stale_poll.journal.entries.at(kSession).phase == ReconcilePhase::kFailed);
}

void CompletedSessionRemainsBoundToItsOriginalAccount() {
  Fixture f;
  ReconcileEntry entry;
  entry.session = CleanSession();
  entry.account_token = "account-a";
  entry.phase = ReconcilePhase::kCompleted;
  f.journal.entries[kSession] = entry;
  f.account = "account-b";
  auto reconciler = f.Reconciler();
  const auto result = reconciler.Reconcile(f.records);
  CHECK(Status(result, kSession)->state == SessionReconcileState::kFailed);
  CHECK(Status(result, kSession)->code == "completed-account-mismatch");
  CHECK(f.journal.entries.at(kSession).phase == ReconcilePhase::kCompleted);
  CHECK(f.journal.entries.at(kSession).account_token == "account-a");
  CHECK(f.cloud.starts == 0 && f.cloud.polls == 0 && f.cloud.cancels == 0);
}

void AccountRestorationPreservesRecoveryAndCompletionGates() {
  Fixture recovery;
  auto interrupted = CleanSession();
  interrupted.phase = WrapperSessionPhase::kFailed;
  interrupted.exit_code.reset();
  recovery.source.next = SessionEnumeration::Success({interrupted});
  auto reconciler = recovery.Reconciler();
  CHECK(Status(reconciler.Reconcile(recovery.records), kSession)->state ==
        SessionReconcileState::kNeedsRecoveryConfirmation);
  recovery.account = "account-b";
  CHECK(Status(reconciler.Reconcile(recovery.records), kSession)->state ==
        SessionReconcileState::kFailed);
  CHECK(recovery.journal.entries.at(kSession).phase ==
        ReconcilePhase::kNeedsRecoveryConfirmation);
  recovery.account = "account-a";
  CHECK(Status(reconciler.Reconcile(recovery.records), kSession)->state ==
        SessionReconcileState::kNeedsRecoveryConfirmation);
  CHECK(recovery.cloud.starts == 0);

  Fixture completion;
  ReconcileEntry entry;
  entry.session = CleanSession();
  entry.account_token = "account-a";
  entry.phase = ReconcilePhase::kCompletionPending;
  entry.operation_id = "operation-1";
  completion.journal.entries[kSession] = entry;
  completion.account = "account-b";
  auto completion_reconciler = completion.Reconciler();
  CHECK(Status(completion_reconciler.Reconcile(completion.records), kSession)
            ->state == SessionReconcileState::kFailed);
  CHECK(completion.journal.entries.at(kSession).phase ==
        ReconcilePhase::kCompletionPending);
  completion.account = "account-a";
  CHECK(Status(completion_reconciler.Reconcile(completion.records), kSession)
            ->state == SessionReconcileState::kCompleted);
  CHECK(completion.cloud.starts == 0 && completion.cloud.polls == 0);
}

void AccountAndActivityAreRecheckedAfterStartAndPoll() {
  Fixture after_start;
  int account_reads = 0;
  auto dependencies = after_start.Dependencies();
  dependencies.current_account_token = [&] {
    return ++account_reads < 3 ? std::string("account-a")
                               : std::string("account-b");
  };
  SessionReconciler start_reconciler(after_start.source, after_start.journal,
                                     after_start.cloud, dependencies);
  CHECK(Status(start_reconciler.Reconcile(after_start.records), kSession)->state ==
        SessionReconcileState::kFailed);
  CHECK(after_start.cloud.starts == 1 && after_start.cloud.cancels == 1);
  CHECK(after_start.journal.entries.at(kSession).phase ==
        ReconcilePhase::kDispatchPending);
  auto restored_start = after_start.Reconciler();
  CHECK(Status(restored_start.Reconcile(after_start.records), kSession)->state ==
        SessionReconcileState::kCloudRunning);
  CHECK(after_start.cloud.starts == 2);

  Fixture after_poll;
  ReconcileEntry entry;
  entry.session = CleanSession();
  entry.account_token = "account-a";
  entry.phase = ReconcilePhase::kCloudRunning;
  entry.operation_id = "operation-1";
  after_poll.journal.entries[kSession] = entry;
  int activity_reads = 0;
  auto poll_dependencies = after_poll.Dependencies();
  poll_dependencies.activity = [&](const std::string &) {
    return ++activity_reads < 2 ? NativeActivity::kStopped
                                : NativeActivity::kActive;
  };
  SessionReconciler poll_reconciler(after_poll.source, after_poll.journal,
                                    after_poll.cloud, poll_dependencies);
  CHECK(Status(poll_reconciler.Reconcile(after_poll.records), kSession)->state ==
        SessionReconcileState::kBlockedByActivity);
  CHECK(after_poll.cloud.polls == 1 && after_poll.cloud.cancels == 1);
  CHECK(after_poll.journal.entries.at(kSession).phase ==
        ReconcilePhase::kDispatchPending);
  auto restored_poll = after_poll.Reconciler();
  CHECK(Status(restored_poll.Reconcile(after_poll.records), kSession)->state ==
        SessionReconcileState::kCloudRunning);
  CHECK(after_poll.cloud.starts == 1);
}

void CloudFailureAndConfirmationRemainIncomplete() {
  Fixture failed;
  failed.cloud.start_result =
      CloudOperationResult::Failed("operation-1", kSession, "account-a",
                                   "offline", "Cloud is offline");
  auto failed_reconciler = failed.Reconciler();
  CHECK(Status(failed_reconciler.Reconcile(failed.records), kSession)->state ==
        SessionReconcileState::kFailed);
  CHECK(failed.journal.entries.at(kSession).phase == ReconcilePhase::kFailed);

  Fixture confirmation;
  confirmation.cloud.start_result = CloudOperationResult::NeedsConfirmation(
      "operation-1", kSession, "account-a", "cloud-conflict",
      "Choose local or Cloud saves");
  auto confirmation_reconciler = confirmation.Reconciler();
  CHECK(Status(confirmation_reconciler.Reconcile(confirmation.records), kSession)
            ->state == SessionReconcileState::kNeedsCloudConfirmation);
  CHECK(confirmation.journal.entries.at(kSession).phase ==
        ReconcilePhase::kCloudRunning);
  confirmation.cloud.poll_result =
      CloudOperationResult::Succeeded("operation-1", kSession, "account-a");
  CHECK(Status(confirmation_reconciler.Reconcile(confirmation.records), kSession)
            ->state == SessionReconcileState::kCompleted);
  CHECK(confirmation.cloud.starts == 1 && confirmation.cloud.polls == 1);
}

void PersistenceFailuresNeverStartOrSilentlyComplete() {
  Fixture before_start;
  before_start.journal.fail_save_call = 1;
  auto first = before_start.Reconciler();
  CHECK(Status(first.Reconcile(before_start.records), kSession)->state ==
        SessionReconcileState::kPersistenceFailed);
  CHECK(before_start.cloud.starts == 0 && before_start.journal.entries.empty());

  Fixture after_success;
  after_success.cloud.start_result =
      CloudOperationResult::Succeeded("operation-1", kSession, "account-a");
  after_success.journal.fail_save_call = 3;
  auto reconciler = after_success.Reconciler();
  CHECK(Status(reconciler.Reconcile(after_success.records), kSession)->state ==
        SessionReconcileState::kPersistenceFailed);
  CHECK(after_success.journal.entries.at(kSession).phase ==
        ReconcilePhase::kCompletionPending);
  CHECK(after_success.cloud.starts == 1);

  after_success.journal.fail_save_call = 0;
  const auto retried = reconciler.Reconcile(after_success.records);
  CHECK(Status(retried, kSession)->state == SessionReconcileState::kCompleted);
  CHECK(after_success.cloud.starts == 1 && after_success.cloud.polls == 0);
}

void EnumerationAndIdentityBoundsFailClosed() {
  Fixture too_many;
  too_many.source.next.sessions.assign(SessionReconciler::kMaxSessionsPerInstall + 1,
                                       CleanSession());
  auto bounded = too_many.Reconciler();
  CHECK(bounded.Reconcile(too_many.records).unresolved_records == 1);
  CHECK(too_many.cloud.starts == 0);

  Fixture oversized;
  auto session = CleanSession();
  session.runtime_bundle_id.assign(SessionReconciler::kMaxIdentityBytes + 1, 'x');
  oversized.source.next = SessionEnumeration::Success({session});
  auto identities = oversized.Reconciler();
  CHECK(identities.Reconcile(oversized.records).rejected_sessions == 1);
  CHECK(oversized.cloud.starts == 0);

  Fixture malformed_journal;
  ReconcileEntry entry;
  entry.session = CleanSession();
  entry.account_token = "account-a";
  entry.phase = ReconcilePhase::kCloudRunning;
  entry.operation_id.assign(SessionReconciler::kMaxIdentityBytes + 1, 'o');
  malformed_journal.journal.entries[kSession] = entry;
  auto journal_bounds = malformed_journal.Reconciler();
  CHECK(Status(journal_bounds.Reconcile(malformed_journal.records), kSession)
            ->state == SessionReconcileState::kRejected);
  CHECK(malformed_journal.cloud.polls == 0 &&
        malformed_journal.cloud.cancels == 0);
}

} // namespace

int main() {
  ActualWrapperRecordsAreAccepted();
  CleanExitUsesActualBottleAndCompletesOnlyAfterTerminalCloudResult();
  HomeOriginIsAcceptedAndBoundToCloudRequest();
  InterruptedAndUncleanSessionsRequireRecoveryConfirmation();
  StartingOrRunningAfterStopIsInterruptedAndMissingEvidenceStaysUnresolved();
  ActiveAndUnknownTitlesNeverDispatchOrPollAndCancelOutstandingWork();
  DuplicatesAndRestartedCompletedSessionsNeverQueueAgain();
  IdentitySpoofsAndUnownedOrUnreadyRecordsAreRejected();
  AccountChangesAndStaleCloudResultsAreCanceledAndNeverComplete();
  CompletedSessionRemainsBoundToItsOriginalAccount();
  AccountRestorationPreservesRecoveryAndCompletionGates();
  AccountAndActivityAreRecheckedAfterStartAndPoll();
  CloudFailureAndConfirmationRemainIncomplete();
  PersistenceFailuresNeverStartOrSilentlyComplete();
  EnumerationAndIdentityBoundsFailClosed();
  std::puts("runtime session reconciler tests passed");
}
