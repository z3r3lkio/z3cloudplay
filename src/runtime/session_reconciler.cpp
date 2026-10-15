#include "runtime/session_reconciler.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace playden::runtime {
namespace {

bool ValidUuid(const std::string &value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-')
    return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    if (!std::isdigit(static_cast<unsigned char>(value[i])) &&
        (value[i] < 'a' || value[i] > 'f'))
      return false;
  }
  return true;
}

bool Bounded(const std::string &value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum;
}

bool SafeAbsolutePath(const std::string &value, std::size_t maximum) {
  if (!Bounded(value, maximum) || value.front() != '/') return false;
  std::size_t begin = 1;
  while (begin <= value.size()) {
    const auto end = value.find('/', begin);
    const auto count = (end == std::string::npos ? value.size() : end) - begin;
    if ((count == 1 && value[begin] == '.') ||
        (count == 2 && value[begin] == '.' && value[begin + 1] == '.'))
      return false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

bool SameIdentity(const WrapperSession &left, const WrapperSession &right) {
  return left.schema_version == right.schema_version &&
         left.session_id == right.session_id && left.origin == right.origin &&
         left.install_id == right.install_id && left.title_id == right.title_id &&
         left.runtime_bundle_id == right.runtime_bundle_id &&
         left.settings_revision == right.settings_revision;
}

bool BoundToRecord(const WrapperSession &session, const RuntimeInstall &record) {
  return session.schema_version == 1 && ValidUuid(session.session_id) &&
         Bounded(session.install_id, SessionReconciler::kMaxIdentityBytes) &&
         Bounded(session.title_id, SessionReconciler::kMaxIdentityBytes) &&
         Bounded(session.runtime_bundle_id,
                 SessionReconciler::kMaxIdentityBytes) &&
         session.install_id == record.install_id &&
         session.title_id == record.title_id &&
         session.runtime_bundle_id == record.runtime_bundle_id &&
         session.settings_revision == record.settings_revision &&
         SafeAbsolutePath(record.data_root, SessionReconciler::kMaxPathBytes) &&
         SafeAbsolutePath(record.install_path, SessionReconciler::kMaxPathBytes);
}

bool CleanExit(const WrapperSession &session) {
  return session.phase == WrapperSessionPhase::kExited && session.exit_code &&
         *session.exit_code == 0;
}

SessionReconcileStatus Status(const std::string &session_id,
                              SessionReconcileState state, std::string code = {},
                              std::string message = {}) {
  return {session_id, state, std::move(code), std::move(message)};
}

} // namespace

SessionEnumeration SessionEnumeration::Success(
    std::vector<WrapperSession> sessions) {
  return {true, std::move(sessions), {}, {}};
}
SessionEnumeration SessionEnumeration::Failure(std::string code,
                                               std::string message) {
  return {false, {}, std::move(code), std::move(message)};
}

ReconcileJournalResult ReconcileJournalResult::Success() {
  return {true, {}, {}};
}
ReconcileJournalResult ReconcileJournalResult::Failure(std::string code,
                                                       std::string message) {
  return {false, std::move(code), std::move(message)};
}
ReconcileJournalLoad ReconcileJournalLoad::Success(
    std::optional<ReconcileEntry> entry) {
  return {true, std::move(entry), {}, {}};
}
ReconcileJournalLoad ReconcileJournalLoad::Failure(std::string code,
                                                   std::string message) {
  return {false, std::nullopt, std::move(code), std::move(message)};
}

CloudOperationResult CloudOperationResult::Running(std::string operation_id,
                                                   std::string session_id,
                                                   std::string account_token) {
  return {CloudOperationState::kRunning, std::move(operation_id),
          std::move(session_id), std::move(account_token), {}, {}};
}
CloudOperationResult CloudOperationResult::Succeeded(std::string operation_id,
                                                     std::string session_id,
                                                     std::string account_token) {
  return {CloudOperationState::kSucceeded, std::move(operation_id),
          std::move(session_id), std::move(account_token), {}, {}};
}
CloudOperationResult CloudOperationResult::NeedsConfirmation(
    std::string operation_id, std::string session_id,
    std::string account_token, std::string code, std::string message) {
  return {CloudOperationState::kNeedsConfirmation, std::move(operation_id),
          std::move(session_id), std::move(account_token), std::move(code),
          std::move(message)};
}
CloudOperationResult CloudOperationResult::Failed(std::string operation_id,
                                                  std::string session_id,
                                                  std::string account_token,
                                                  std::string code,
                                                  std::string message) {
  return {CloudOperationState::kFailed, std::move(operation_id),
          std::move(session_id), std::move(account_token), std::move(code),
          std::move(message)};
}

SessionReconciler::SessionReconciler(
    ReconcileSessionSource &source, ReconcileJournal &journal,
    ReconcileCloud &cloud, SessionReconcileDependencies dependencies)
    : source_(source), journal_(journal), cloud_(cloud),
      dependencies_(std::move(dependencies)) {}

SessionReconcileStatus SessionReconciler::Persist(
    ReconcileEntry entry, SessionReconcileState state) {
  const auto saved = journal_.Save(entry);
  if (!saved.ok)
    return Status(entry.session.session_id,
                  SessionReconcileState::kPersistenceFailed, saved.code,
                  saved.message);
  return Status(entry.session.session_id, state, entry.code, entry.message);
}

SessionReconcileStatus SessionReconciler::Fail(
    ReconcileEntry entry, std::string code, std::string message,
    SessionReconcileState state) {
  entry.phase = ReconcilePhase::kFailed;
  entry.code = std::move(code);
  entry.message = std::move(message);
  return Persist(std::move(entry), state);
}

SessionReconcileStatus SessionReconciler::Pause(
    ReconcileEntry entry, std::string code, std::string message,
    SessionReconcileState state) {
  entry.phase = ReconcilePhase::kDispatchPending;
  entry.operation_id.clear();
  entry.code = std::move(code);
  entry.message = std::move(message);
  return Persist(std::move(entry), state);
}

ReconcileResult
SessionReconciler::Reconcile(const std::vector<RuntimeInstall> &records) {
  ReconcileResult result;
  std::unordered_set<std::string> seen;
  const auto count = std::min(records.size(), kMaxRecords);
  result.unresolved_records += records.size() - count;
  for (std::size_t index = 0; index < count; ++index) {
    const auto &record = records[index];
    if (record.state != PreparationState::kReady || !dependencies_.owned ||
        !dependencies_.owned(record))
      continue;
    const auto enumerated = source_.Enumerate(record, kMaxSessionsPerInstall);
    if (!enumerated.ok || enumerated.sessions.empty() ||
        enumerated.sessions.size() > kMaxSessionsPerInstall) {
      ++result.unresolved_records;
      continue;
    }
    for (const auto &session : enumerated.sessions) {
      if (!seen.emplace(session.session_id).second) continue;
      if (!BoundToRecord(session, record)) {
        ++result.rejected_sessions;
        result.sessions.push_back(Status(session.session_id,
                                         SessionReconcileState::kRejected,
                                         "session-identity-invalid",
                                         "wrapper session identity is invalid"));
        continue;
      }
      result.sessions.push_back(Process(record, session));
    }
  }
  return result;
}

SessionReconcileStatus SessionReconciler::Process(
    const RuntimeInstall &record, const WrapperSession &session) {
  const auto loaded = journal_.Load(session.session_id);
  if (!loaded.ok)
    return Status(session.session_id, SessionReconcileState::kPersistenceFailed,
                  loaded.code, loaded.message);

  const std::string account = dependencies_.current_account_token
                                  ? dependencies_.current_account_token()
                                  : std::string{};
  if (!Bounded(account, kMaxIdentityBytes))
    return Status(session.session_id, SessionReconcileState::kFailed,
                  "account-unavailable",
                  "current Cloud account identity is unavailable");

  if (loaded.entry) {
    auto entry = *loaded.entry;
    const bool operation_valid =
        entry.phase != ReconcilePhase::kCloudRunning ||
        Bounded(entry.operation_id, kMaxIdentityBytes);
    if (entry.schema_version != 1 ||
        !Bounded(entry.account_token, kMaxIdentityBytes) || !operation_valid ||
        !SameIdentity(entry.session, session))
      return Status(session.session_id, SessionReconcileState::kRejected,
                    "journal-invalid",
                    "reconciliation journal is invalid for this session");
    if (entry.phase == ReconcilePhase::kCompleted) {
      if (entry.account_token != account)
        return Status(session.session_id, SessionReconcileState::kFailed,
                      "completed-account-mismatch",
                      "session was reconciled for a different Cloud account");
      return Status(session.session_id, SessionReconcileState::kCompleted);
    }
    if (entry.account_token != account) {
      if (entry.phase == ReconcilePhase::kCloudRunning &&
          !entry.operation_id.empty()) {
        cloud_.Cancel(entry.operation_id, entry.session.session_id,
                      entry.account_token);
        return Pause(std::move(entry), "account-changed",
                     "Cloud account changed during reconciliation",
                     SessionReconcileState::kFailed);
      }
      return Status(session.session_id, SessionReconcileState::kFailed,
                    "account-changed",
                    "Cloud account changed during reconciliation");
    }
    if (entry.phase == ReconcilePhase::kFailed)
      return Status(session.session_id, SessionReconcileState::kFailed,
                    entry.code, entry.message);
    if (entry.phase == ReconcilePhase::kNeedsRecoveryConfirmation)
      return Status(session.session_id,
                    SessionReconcileState::kNeedsRecoveryConfirmation);
    if (entry.phase == ReconcilePhase::kCompletionPending) {
      entry.phase = ReconcilePhase::kCompleted;
      entry.code.clear();
      entry.message.clear();
      return Persist(std::move(entry), SessionReconcileState::kCompleted);
    }
    if (entry.phase == ReconcilePhase::kCloudRunning)
      return Poll(record, std::move(entry));
    return Dispatch(record, std::move(entry));
  }

  const NativeActivity activity = dependencies_.activity
                                      ? dependencies_.activity(record.title_id)
                                      : NativeActivity::kUnknown;
  if (activity != NativeActivity::kStopped)
    return Status(session.session_id,
                  SessionReconcileState::kBlockedByActivity,
                  activity == NativeActivity::kActive ? "title-active"
                                                      : "title-status-unknown",
                  "Cloud reconciliation waits for confirmed title stop");

  ReconcileEntry entry;
  entry.session = session;
  entry.account_token = account;
  if (!CleanExit(session)) {
    entry.phase = ReconcilePhase::kNeedsRecoveryConfirmation;
    return Persist(std::move(entry),
                   SessionReconcileState::kNeedsRecoveryConfirmation);
  }
  entry.phase = ReconcilePhase::kDispatchPending;
  const auto saved = journal_.Save(entry);
  if (!saved.ok)
    return Status(session.session_id, SessionReconcileState::kPersistenceFailed,
                  saved.code, saved.message);
  return Dispatch(record, std::move(entry));
}

SessionReconcileStatus SessionReconciler::Dispatch(
    const RuntimeInstall &record, ReconcileEntry entry) {
  const auto activity = dependencies_.activity
                            ? dependencies_.activity(record.title_id)
                            : NativeActivity::kUnknown;
  const auto account = dependencies_.current_account_token
                           ? dependencies_.current_account_token()
                           : std::string{};
  if (activity != NativeActivity::kStopped)
    return Status(entry.session.session_id,
                  SessionReconcileState::kBlockedByActivity,
                  activity == NativeActivity::kActive ? "title-active"
                                                      : "title-status-unknown",
                  "Cloud reconciliation waits for confirmed title stop");
  if (account != entry.account_token)
    return Status(entry.session.session_id, SessionReconcileState::kFailed,
                  "account-changed",
                  "Cloud account changed before reconciliation dispatch");

  SessionCloudRequest request;
  request.location.app_id = record.app_id;
  request.location.game_root = record.install_path;
  request.location.state_root = record.data_root + "/cloud";
  request.location.prefix_root = record.data_root + "/library/prefixes/game";
  request.session = entry.session;
  request.account_token = entry.account_token;
  auto cloud_result = cloud_.StartCheck(request);
  return HandleCloudResult(record, std::move(entry), std::move(cloud_result),
                           true);
}

SessionReconcileStatus SessionReconciler::Poll(const RuntimeInstall &record,
                                               ReconcileEntry entry) {
  const auto activity = dependencies_.activity
                            ? dependencies_.activity(record.title_id)
                            : NativeActivity::kUnknown;
  const auto account = dependencies_.current_account_token
                           ? dependencies_.current_account_token()
                           : std::string{};
  if (activity != NativeActivity::kStopped) {
    cloud_.Cancel(entry.operation_id, entry.session.session_id,
                  entry.account_token);
    return Pause(std::move(entry), "activity-changed",
                 "native title activity changed during reconciliation",
                 SessionReconcileState::kBlockedByActivity);
  }
  if (account != entry.account_token) {
    cloud_.Cancel(entry.operation_id, entry.session.session_id,
                  entry.account_token);
    return Pause(std::move(entry), "account-changed",
                 "Cloud account changed during reconciliation",
                 SessionReconcileState::kFailed);
  }
  auto cloud_result = cloud_.Poll(entry.operation_id, entry.session.session_id,
                                  entry.account_token);
  return HandleCloudResult(record, std::move(entry), std::move(cloud_result),
                           false);
}

SessionReconcileStatus SessionReconciler::HandleCloudResult(
    const RuntimeInstall &record, ReconcileEntry entry,
    CloudOperationResult result, bool started_now) {
  const auto activity = dependencies_.activity
                            ? dependencies_.activity(record.title_id)
                            : NativeActivity::kUnknown;
  const auto account = dependencies_.current_account_token
                           ? dependencies_.current_account_token()
                           : std::string{};
  const bool valid_operation = Bounded(result.operation_id, kMaxIdentityBytes);
  const bool valid_binding = valid_operation &&
                             result.session_id == entry.session.session_id &&
                             result.account_token == entry.account_token &&
                             (started_now || result.operation_id == entry.operation_id);
  if (!valid_binding) {
    if (!result.operation_id.empty())
      cloud_.Cancel(result.operation_id, result.session_id, result.account_token);
    return Fail(std::move(entry), "cloud-binding-invalid",
                "Cloud result identity does not match reconciliation",
                SessionReconcileState::kFailed);
  }
  if (activity != NativeActivity::kStopped) {
    cloud_.Cancel(result.operation_id, entry.session.session_id,
                  entry.account_token);
    return Pause(std::move(entry), "activity-changed",
                 "native title activity changed during reconciliation",
                 SessionReconcileState::kBlockedByActivity);
  }
  if (account != entry.account_token) {
    cloud_.Cancel(result.operation_id, entry.session.session_id,
                  entry.account_token);
    return Pause(std::move(entry), "account-changed",
                 "Cloud account changed during reconciliation",
                 SessionReconcileState::kFailed);
  }

  entry.operation_id = result.operation_id;
  entry.code = std::move(result.code);
  entry.message = std::move(result.message);
  if (result.state == CloudOperationState::kFailed) {
    std::string code = entry.code.empty() ? "cloud-failed" : entry.code;
    std::string message = entry.message.empty() ? "Cloud reconciliation failed"
                                                : entry.message;
    return Fail(std::move(entry), std::move(code), std::move(message),
                SessionReconcileState::kFailed);
  }
  if (result.state == CloudOperationState::kRunning ||
      result.state == CloudOperationState::kNeedsConfirmation) {
    entry.phase = ReconcilePhase::kCloudRunning;
    const auto state = result.state == CloudOperationState::kNeedsConfirmation
                           ? SessionReconcileState::kNeedsCloudConfirmation
                           : SessionReconcileState::kCloudRunning;
    const auto saved = journal_.Save(entry);
    if (!saved.ok) {
      if (started_now)
        cloud_.Cancel(entry.operation_id, entry.session.session_id,
                      entry.account_token);
      return Status(entry.session.session_id,
                    SessionReconcileState::kPersistenceFailed, saved.code,
                    saved.message);
    }
    return Status(entry.session.session_id, state, entry.code, entry.message);
  }

  entry.phase = ReconcilePhase::kCompletionPending;
  auto saved = journal_.Save(entry);
  if (!saved.ok)
    return Status(entry.session.session_id,
                  SessionReconcileState::kPersistenceFailed, saved.code,
                  saved.message);
  entry.phase = ReconcilePhase::kCompleted;
  entry.code.clear();
  entry.message.clear();
  saved = journal_.Save(entry);
  if (!saved.ok)
    return Status(entry.session.session_id,
                  SessionReconcileState::kPersistenceFailed, saved.code,
                  saved.message);
  return Status(entry.session.session_id, SessionReconcileState::kCompleted);
}

ReconcileJournalResult
SessionReconciler::ConfirmRecovery(const std::string &session_id,
                                   const std::string &account_token) {
  if (!ValidUuid(session_id) || !Bounded(account_token, kMaxIdentityBytes))
    return ReconcileJournalResult::Failure(
        "confirmation-invalid", "recovery confirmation identity is invalid");
  const auto current = dependencies_.current_account_token
                           ? dependencies_.current_account_token()
                           : std::string{};
  if (current != account_token)
    return ReconcileJournalResult::Failure(
        "account-changed", "Cloud account changed before recovery confirmation");
  const auto loaded = journal_.Load(session_id);
  if (!loaded.ok)
    return ReconcileJournalResult::Failure(loaded.code, loaded.message);
  if (!loaded.entry || loaded.entry->account_token != account_token ||
      loaded.entry->phase != ReconcilePhase::kNeedsRecoveryConfirmation)
    return ReconcileJournalResult::Failure(
        "confirmation-not-pending", "session is not awaiting recovery confirmation");
  auto entry = *loaded.entry;
  entry.phase = ReconcilePhase::kDispatchPending;
  entry.code.clear();
  entry.message.clear();
  return journal_.Save(entry);
}

} // namespace playden::runtime
