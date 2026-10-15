#pragma once

#include "platform/storage.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace playden::runtime {

enum class PublicationRecoveryState { kAbsent, kValid, kCorrupt, kUnavailable };
inline constexpr std::size_t kMaxPublicationRegistryBackupBytes = 32 * 1024 * 1024;

// The operation journal freezes this binding, including the original registry
// mode. Backup files themselves are always exclusive, app-owned 0600 files.
struct RegistryBackupBinding {
  std::string filename;
  unsigned source_mode = 0;
  std::uint64_t size = 0;
  std::string sha256;
};
struct PublicationBackupLoad {
  PublicationRecoveryState state = PublicationRecoveryState::kUnavailable;
  std::string bytes;
  std::string message;
};

// Requires an already-created private control directory and a publisher lease
// held by the caller. Never creates directories, adopts an existing backup or
// removes an orphan. A failure may leave a file; stop and reconcile before any
// later journal/receipt/live-file effects. An orphan grants no ownership.
Result<RegistryBackupBinding> CreatePublicationRegistryBackup(
    const std::filesystem::path& control_directory, const std::string& transaction_id,
    std::string_view bytes, unsigned source_mode);
PublicationBackupLoad LoadPublicationRegistryBackup(
    const std::filesystem::path& control_directory, const std::string& transaction_id,
    const RegistryBackupBinding& binding);

}  // namespace playden::runtime
