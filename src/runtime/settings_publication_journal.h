#pragma once
#include "runtime/publication_transaction.h"
#include "runtime/game_settings.h"
#include "runtime/game_install.h"
#include "runtime/publication_backup.h"
#include "runtime/package_installer.h"
#include <optional>

namespace playden::runtime {
inline constexpr std::size_t kMaxSettingsPublicationOperationBytes = 2 * 1024 * 1024;
// Frozen user request, durable before control setup or before-state capture.
// It grants no ownership, receipt authority or readiness; OWNER/kit/activity and
// the complete captured operation must still be independently admitted.
struct SettingsPublicationRequest {
  unsigned schema = 1;
  std::string transaction_id, kit_id;
  RuntimeInstall install;
  SettingsLaunchPlan plan;
  std::int64_t registry_unix_time = 0;
};
std::optional<Bytes> EncodeSettingsPublicationRequest(const SettingsPublicationRequest&, std::string* reason);
std::optional<SettingsPublicationRequest> DecodeSettingsPublicationRequest(const Bytes&, std::string* reason);
struct PublicationRequestLoad {
  PublicationRecoveryState state = PublicationRecoveryState::kUnavailable;
  std::optional<SettingsPublicationRequest> request;
  std::string message;
};
// Existing private OWNER request directory only, with serialized worker access.
// Requests are created exclusively, synced and read back. A pre-arm orphan
// remains inert; it cannot authorize setup, capture, registration or readiness.
Result<void> CreateSettingsPublicationRequest(const std::filesystem::path& request_directory,
    const SettingsPublicationRequest&);
PublicationRequestLoad LoadSettingsPublicationRequest(const std::filesystem::path& request_directory,
    const std::string& transaction_id);
struct FrozenFileSnapshot {
  bool present = false;
  unsigned mode = 0;
  std::uint64_t size = 0;
  std::string sha256;
  std::string bytes;
};
struct SettingsPublicationOperation {
  unsigned schema = 1;
  std::string transaction_id, kit_id;
  std::uint64_t previous_revision = 0;
  RuntimeInstall install;
  SettingsLaunchPlan plan;
  std::string descriptor_bytes, catalog_bytes, metadata_bytes;
  // Captured from the previous verified committed set before replacing its
  // receipt with pending. Retains ownership hashes for partially updated files.
  std::string previous_receipt_bytes, previous_metadata_bytes;
  // Makes registry section timestamps deterministic across interrupted retries.
  std::int64_t registry_unix_time = 0;
  FrozenFileSnapshot dxvk_before;
  std::optional<RegistryBackupBinding> registry_backup;
  std::string plan_sha256, pending_receipt_bytes, committed_receipt_bytes;
};
// PWOP2 wire format; older local draft encodings are refused. The semantic
// operation schema remains 1. Pure frozen-byte contract. No recipe reload, filesystem mutation or ownership
// grant. Profile/input bytes live only in plan. Receipts and the digest are
// generated from the validated canonical payload, avoiding a hash cycle.
std::optional<SettingsPublicationOperation> FreezeSettingsPublicationOperation(
    SettingsPublicationOperation operation, std::string* reason);
std::optional<Bytes> EncodeSettingsPublicationOperation(
    const SettingsPublicationOperation& operation, std::string* reason);
std::optional<SettingsPublicationOperation> DecodeSettingsPublicationOperation(
    const Bytes& bytes, std::string* reason);
// Rebuilds the publication from validated frozen identities/profile/input bytes.
// The parser-validation flag is derived here; it is never serialized or trusted.
std::optional<ProfilePublication> RebuildFrozenProfilePublication(
    const SettingsPublicationOperation& operation, std::string* reason);
struct PublicationOperationLoad {
  PublicationRecoveryState state = PublicationRecoveryState::kUnavailable;
  std::optional<SettingsPublicationOperation> operation;
  std::string message;
};
// Existing private control directory only; caller holds the publisher lease.
// The immutable settings-operation.v1 is created exclusively after validating
// every referenced backup. Failures may leave files and require fresh recovery.
// Missing referenced backups are corruption, never optional ownership proof.
Result<void> CreateSettingsPublicationOperation(
    const std::filesystem::path& control_directory, const SettingsPublicationOperation& operation);
PublicationOperationLoad LoadSettingsPublicationOperation(const std::filesystem::path& control_directory);
// Caller must first verify committed semantics and durable Ready OWNER state
// under its exclusive lease. Moves the exact journal to a transaction-specific
// immutable archive without replacing a destination; preserves backups. Retry
// admits only the exact archived bytes if the active name is already absent.
// after_rename is a test-only crash seam; production leaves it empty.
Result<void> ArchiveSettingsPublicationOperation(const std::filesystem::path& control_directory,
    const SettingsPublicationOperation& operation, const std::function<bool()>& after_rename = {});
// Durable capture handoff: immutable transaction-specific intent, separate from
// the active publication journal. Caller holds the qualified publisher lease.
// Creation is exclusive; recovery requires exact private policy/codec/backups
// and transaction-to-filename binding. Intent alone never grants readiness.
Result<void> CreatePreparedSettingsPublicationOperation(const std::filesystem::path& control_directory,
    const SettingsPublicationOperation& operation);
PublicationOperationLoad LoadPreparedSettingsPublicationOperation(const std::filesystem::path& control_directory,
    const std::string& transaction_id);
}  // namespace playden::runtime
