#pragma once

#include "runtime/install_service.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playden::runtime {

enum class SessionOrigin { kHome, kPlayden };
enum class WrapperSessionPhase { kStarting, kRunning, kFailed, kExited };

struct WrapperSession {
  std::uint32_t schema_version = 1;
  std::string session_id;
  SessionOrigin origin = SessionOrigin::kPlayden;
  std::string install_id;
  std::string title_id;
  std::string runtime_bundle_id;
  std::uint64_t settings_revision = 0;
  WrapperSessionPhase phase = WrapperSessionPhase::kStarting;
  std::optional<int> exit_code;
};

struct SessionEnumeration {
  bool ok = false;
  std::vector<WrapperSession> sessions;
  std::string code;
  std::string message;

  static SessionEnumeration Success(std::vector<WrapperSession> sessions);
  static SessionEnumeration Failure(std::string code, std::string message);
};

// The adapter validates file ownership, size and syntax before returning
// records. SessionReconciler independently binds the actual persisted wrapper
// identities (session/install/title/runtime/settings) to the runtime record
// before allowing Cloud work. App ownership comes from that owned record; the
// wrapper session-v1 writer does not persist an independent app_id field.
class ReconcileSessionSource {
public:
  virtual ~ReconcileSessionSource() = default;
  virtual SessionEnumeration Enumerate(const RuntimeInstall &record,
                                       std::size_t limit) = 0;
};

enum class ReconcilePhase {
  kNeedsRecoveryConfirmation,
  kDispatchPending,
  kCloudRunning,
  kCompletionPending,
  kCompleted,
  kFailed,
};

struct ReconcileEntry {
  std::uint32_t schema_version = 1;
  WrapperSession session;
  // Opaque identity/revision token. It is not a credential.
  std::string account_token;
  ReconcilePhase phase = ReconcilePhase::kNeedsRecoveryConfirmation;
  std::string operation_id;
  std::string code;
  std::string message;
};

struct ReconcileJournalResult {
  bool ok = false;
  std::string code;
  std::string message;
  static ReconcileJournalResult Success();
  static ReconcileJournalResult Failure(std::string code, std::string message);
};

struct ReconcileJournalLoad {
  bool ok = false;
  std::optional<ReconcileEntry> entry;
  std::string code;
  std::string message;
  static ReconcileJournalLoad Success(std::optional<ReconcileEntry> entry);
  static ReconcileJournalLoad Failure(std::string code, std::string message);
};

// Save atomically replaces an owner-private per-session record. The interface
// deliberately makes no power-loss durability or network exactly-once claim.
class ReconcileJournal {
public:
  virtual ~ReconcileJournal() = default;
  virtual ReconcileJournalLoad Load(const std::string &session_id) = 0;
  virtual ReconcileJournalResult Save(const ReconcileEntry &entry) = 0;
};

struct SessionCloudLocation {
  std::uint32_t app_id = 0;
  std::string game_root;
  std::string state_root;
  std::string prefix_root;
};

struct SessionCloudRequest {
  SessionCloudLocation location;
  WrapperSession session;
  std::string account_token;
};

enum class CloudOperationState { kRunning, kSucceeded, kNeedsConfirmation, kFailed };

struct CloudOperationResult {
  CloudOperationState state = CloudOperationState::kFailed;
  std::string operation_id;
  std::string session_id;
  std::string account_token;
  std::string code;
  std::string message;

  static CloudOperationResult Running(std::string operation_id,
                                      std::string session_id,
                                      std::string account_token);
  static CloudOperationResult Succeeded(std::string operation_id,
                                        std::string session_id,
                                        std::string account_token);
  static CloudOperationResult NeedsConfirmation(std::string operation_id,
                                                std::string session_id,
                                                std::string account_token,
                                                std::string code,
                                                std::string message);
  static CloudOperationResult Failed(std::string operation_id,
                                     std::string session_id,
                                     std::string account_token,
                                     std::string code,
                                     std::string message);
};

// StartCheck is check-only by default. An adapter that can apply or upload must
// itself recheck the bound account and authoritative title inactivity directly
// before every transfer; SessionReconciler also checks around every call.
class ReconcileCloud {
public:
  virtual ~ReconcileCloud() = default;
  virtual CloudOperationResult StartCheck(const SessionCloudRequest &request) = 0;
  virtual CloudOperationResult Poll(const std::string &operation_id,
                                    const std::string &session_id,
                                    const std::string &account_token) = 0;
  virtual void Cancel(const std::string &operation_id,
                      const std::string &session_id,
                      const std::string &account_token) = 0;
};

enum class SessionReconcileState {
  kNeedsRecoveryConfirmation,
  kCloudRunning,
  kNeedsCloudConfirmation,
  kCompleted,
  kBlockedByActivity,
  kPersistenceFailed,
  kRejected,
  kFailed,
};

struct SessionReconcileStatus {
  std::string session_id;
  SessionReconcileState state = SessionReconcileState::kFailed;
  std::string code;
  std::string message;
};

struct ReconcileResult {
  std::vector<SessionReconcileStatus> sessions;
  std::size_t unresolved_records = 0;
  std::size_t rejected_sessions = 0;
};

struct SessionReconcileDependencies {
  std::function<NativeActivity(const std::string &title_id)> activity;
  std::function<std::string()> current_account_token;
  std::function<bool(const RuntimeInstall &record)> owned;
};

class SessionReconciler {
public:
  static constexpr std::size_t kMaxRecords = 256;
  static constexpr std::size_t kMaxSessionsPerInstall = 256;
  static constexpr std::size_t kMaxIdentityBytes = 128;
  static constexpr std::size_t kMaxPathBytes = 1024;

  SessionReconciler(ReconcileSessionSource &source, ReconcileJournal &journal,
                    ReconcileCloud &cloud,
                    SessionReconcileDependencies dependencies);

  ReconcileResult Reconcile(const std::vector<RuntimeInstall> &records);
  ReconcileJournalResult ConfirmRecovery(const std::string &session_id,
                                         const std::string &account_token);

private:
  SessionReconcileStatus Process(const RuntimeInstall &record,
                                 const WrapperSession &session);
  SessionReconcileStatus Dispatch(const RuntimeInstall &record,
                                  ReconcileEntry entry);
  SessionReconcileStatus Poll(const RuntimeInstall &record,
                              ReconcileEntry entry);
  SessionReconcileStatus HandleCloudResult(const RuntimeInstall &record,
                                           ReconcileEntry entry,
                                           CloudOperationResult result,
                                           bool started_now);
  SessionReconcileStatus Fail(ReconcileEntry entry, std::string code,
                              std::string message,
                              SessionReconcileState state);
  SessionReconcileStatus Pause(ReconcileEntry entry, std::string code,
                               std::string message,
                               SessionReconcileState state);
  SessionReconcileStatus Persist(ReconcileEntry entry,
                                 SessionReconcileState state);

  ReconcileSessionSource &source_;
  ReconcileJournal &journal_;
  ReconcileCloud &cloud_;
  SessionReconcileDependencies dependencies_;
};

} // namespace playden::runtime
