#pragma once

#include "platform/storage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace playden::runtime {

enum class PreparationState {
  kDownloaded,
  kPreparing,
  kRegistering,
  kReady,
  kFailed,
  kUnavailable,
  kRemoving,
};

enum class TitleAvailability { kFree, kOccupied, kUnknown };

struct NativeTitleAvailability {
  TitleAvailability inventory = TitleAvailability::kUnknown;
  TitleAvailability source_path = TitleAvailability::kUnknown;
};

using NativeAvailabilityQuery =
    std::function<NativeTitleAvailability(const std::string& title_id)>;

struct RuntimeArtifactIdentity {
  std::string artifact_id;
  std::string identity;
  std::string sha256;
};

struct RuntimeFailure {
  std::string code;
  std::string message;
};

struct PreparationJournal {
  std::string operation_id;
  std::string operation;
  std::string step;
};

struct RuntimeInstall {
  std::uint32_t schema_version = 1;
  std::string install_id;
  std::string title_id;
  std::uint32_t app_id = 0;
  std::string data_root;
  std::string default_profile_id;
  std::string runtime_bundle_id;
  std::string recipe_id;
  std::string recipe_version;
  std::uint64_t settings_revision = 0;
  std::string exit_policy = "exit-title";

  std::string launch_entry_id;
  std::string launch_path;
  std::string launch_arguments;
  std::string launch_working_directory;
  std::string install_path;
  std::string volume_id;
  PreparationState state = PreparationState::kDownloaded;
  std::vector<RuntimeArtifactIdentity> artifacts;
  std::optional<RuntimeFailure> last_failure;
  std::optional<PreparationJournal> journal;
};

// Shared OWNER record validation; this grants no filesystem ownership or readiness.
bool ValidateRuntimeInstallRecord(const RuntimeInstall& record);
// Same install: install, title and app ids, data root and game folder. Runtime
// artifacts, recipe and launch fields are not compared (a kit respin may change
// them). State, revision, journal and failure are checked separately.
bool SameRuntimeInstallIdentity(const RuntimeInstall& first, const RuntimeInstall& second);

struct ReserveRequest {
  std::string install_id;
  std::uint32_t app_id = 0;
  std::string default_profile_id;
  std::string runtime_bundle_id;
  std::string recipe_id;
  std::string recipe_version;
  std::uint64_t settings_revision = 0;
  std::string launch_entry_id;
  std::string launch_path;
  std::string launch_arguments;
  std::string launch_working_directory;
  std::string install_path;
  std::string volume_id;
  std::vector<std::string> kit_title_variants;
};

struct LegacyInstall {
  std::string install_id;
  std::uint32_t app_id = 0;
  std::string install_path;
  std::string volume_id;
  std::string launch_entry_id;
  std::string launch_path;
  std::string launch_arguments;
  std::string launch_working_directory;
  std::string default_profile_id;
  std::string runtime_bundle_id;
  std::string recipe_id;
  std::string recipe_version;
  std::vector<std::string> kit_title_variants;
};

enum class RepositoryError {
  kNone,
  kNotLoaded,
  kIo,
  kMalformed,
  kUnsupportedSchema,
  kBounds,
  kInvalidRecord,
  kConflict,
  kNotFound,
  kTitleVariantsExhausted,
  kRemovalIncomplete,
};

template <typename T>
class RepositoryResult {
 public:
  static RepositoryResult Success(T value) {
    return RepositoryResult(std::move(value), RepositoryError::kNone, {});
  }
  static RepositoryResult Failure(RepositoryError error, std::string message) {
    return RepositoryResult(std::nullopt, error, std::move(message));
  }
  bool ok() const { return value_.has_value(); }
  const T& value() const { return value_.value(); }
  T& value() { return value_.value(); }
  RepositoryError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  RepositoryResult(T value, RepositoryError error, std::string message)
      : value_(std::move(value)), error_(error), message_(std::move(message)) {}
  RepositoryResult(std::nullopt_t, RepositoryError error, std::string message)
      : value_(std::nullopt), error_(error), message_(std::move(message)) {}
  std::optional<T> value_;
  RepositoryError error_;
  std::string message_;
};

template <>
class RepositoryResult<void> {
 public:
  static RepositoryResult Success() {
    return RepositoryResult(true, RepositoryError::kNone, {});
  }
  static RepositoryResult Failure(RepositoryError error, std::string message) {
    return RepositoryResult(false, error, std::move(message));
  }
  bool ok() const { return ok_; }
  RepositoryError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  RepositoryResult(bool ok, RepositoryError error, std::string message)
      : ok_(ok), error_(error), message_(std::move(message)) {}
  bool ok_;
  RepositoryError error_;
  std::string message_;
};

class RuntimeInstallRepository {
 public:
  static constexpr std::uint32_t kSchemaVersion = 1;
  static constexpr std::size_t kMaxFileBytes = 256 * 1024;

  RuntimeInstallRepository(Storage& storage, std::string relative_path,
                           NativeAvailabilityQuery native_availability);

  RepositoryResult<void> Load();
  RepositoryResult<void> Save();
  std::vector<RuntimeInstall> Snapshot() const;
  std::optional<RuntimeInstall> Find(const std::string& install_id) const;
  RepositoryResult<RuntimeInstall> Reserve(const ReserveRequest& request);
  RepositoryResult<RuntimeInstall> Adopt(const LegacyInstall& legacy);
  RepositoryResult<void> Update(const RuntimeInstall& record);
  RepositoryResult<void> EraseAfterUnregister(const std::string& install_id);
  // Same, with the caller's fresh observation instead of the availability query:
  // removal confirms with the console inventory and the source folder while
  // ShadowMount's cache is stale. Both must be free; nothing else is relaxed.
  RepositoryResult<void> EraseAfterUnregister(const std::string& install_id,
                                              const NativeTitleAvailability& confirmed);

 private:
  RepositoryResult<void> Persist();

  Storage& storage_;
  std::string relative_path_;
  NativeAvailabilityQuery native_availability_;
  bool loaded_ = false;
  std::unordered_map<std::string, RuntimeInstall> installs_;
  std::unordered_map<std::string, std::string> reservations_;
};

}  // namespace playden::runtime
