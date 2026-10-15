#include "platform/storage.h"
#include "platform/managed_directory.h"

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace z3cloudplay {
namespace {
using platform::DirectoryPolicy;
using platform::ManagedDirectory;

std::optional<ManagedDirectory> Parent(const std::string& root,
                                       const std::string& relative_path) {
  auto directory = ManagedDirectory::Open(root, DirectoryPolicy::kPrivate);
  if (!directory) return std::nullopt;
  for (const auto& part : std::filesystem::path(relative_path).parent_path()) {
    auto child = directory->OpenChild(part.string(), DirectoryPolicy::kPrivate);
    if (!child) return std::nullopt;
    directory = std::move(child);
  }
  return directory;
}
Result<void> Failure(StorageError error, const char* message) {
  return Result<void>::Failure(error, message);
}
}

Result<Bytes> Storage::ReadDurable(const std::string& relative_path,
                                  std::size_t max_bytes) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) return Result<Bytes>::Failure(resolved.error(), resolved.message());
  if (!max_bytes || max_bytes > 32 * 1024 * 1024)
    return Result<Bytes>::Failure(StorageError::kReadFailed, "Invalid bounded journal read.");
  auto parent = Parent(root_, relative_path);
  if (!parent) return Result<Bytes>::Failure(StorageError::kOpenFailed, "Private journal directory is unavailable.");
  const auto name = std::filesystem::path(relative_path).filename().string();
  const int fd = parent->OpenRegularFile(name, true);
  if (fd < 0) {
    const int error = errno;
    if (error == ENOENT && parent->Sync())
      return Result<Bytes>::Failure(StorageError::kNotFound, "Journal is absent.");
    return Result<Bytes>::Failure(StorageError::kOpenFailed, "Journal could not be opened or its absence synced.");
  }
  struct stat info{};
  // Legacy registries used 0644; accept them for migration, publish new files 0600.
  bool valid = fstat(fd, &info) == 0 &&
      ((info.st_mode & 07777) == 0600 || (info.st_mode & 07777) == 0644) &&
      info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) <= max_bytes;
  Bytes bytes;
  std::uint8_t buffer[4096];
  while (valid) {
    const auto count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || static_cast<std::size_t>(count) > max_bytes - bytes.size()) {
      valid = false; break;
    }
    if (!count) break;
    bytes.insert(bytes.end(), buffer, buffer + count);
  }
  // Re-establish durability before accepting a journal visible after a failed
  // publication sync, including legacy files. Cooperating writers are serialized.
  const bool synced = valid && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!valid) return Result<Bytes>::Failure(StorageError::kReadFailed, "Journal is unreadable, oversized or has unsupported permissions.");
  if (!synced || !closed || !parent->Sync())
    return Result<Bytes>::Failure(StorageError::kSyncFailed, "Journal recovery sync failed.");
  return Result<Bytes>::Success(std::move(bytes));
}

Result<void> Storage::WriteDurable(const std::string& relative_path,
                                   const Bytes& bytes) const {
  return WriteDurableImpl(relative_path, bytes, true);
}

Result<void> Storage::WriteDurableExclusive(const std::string& relative_path,
                                            const Bytes& bytes) const {
  return WriteDurableImpl(relative_path, bytes, false);
}

Result<void> Storage::WriteDurableImpl(const std::string& relative_path,
                                       const Bytes& bytes,
                                       bool replace_existing) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) return Failure(resolved.error(), "Invalid journal path.");
  auto parent = Parent(root_, relative_path);
  if (!parent) return Failure(StorageError::kOpenFailed, "Private journal directory is unavailable.");
  const auto name = std::filesystem::path(relative_path).filename().string();
  if (!replace_existing) {
    struct stat existing{};
    if (parent->InspectChild(name, &existing) || errno != ENOENT)
      return Failure(StorageError::kOpenFailed, "Immutable journal destination exists or is unsafe.");
  }
  static std::atomic<unsigned long> sequence{0};
  std::string temporary;
  int fd = -1;
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    temporary = "." + name + ".journal-" + std::to_string(getpid()) + "-" + std::to_string(sequence++);
    fd = parent->CreateExclusiveFile(temporary, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) return Failure(StorageError::kOpenFailed, "Exclusive private journal creation failed.");
  std::size_t offset = 0;
  bool valid = true;
  while (offset < bytes.size()) {
    const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { valid = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  const bool synced = valid && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  // Failed temporary files remain for explicit recovery. Never follow/remove
  // an old .tmp or clean through a possibly replaced context.
  if (!valid || !synced || !closed)
    return Failure(StorageError::kWriteFailed, "Journal write, file sync or close failed; temporary file may remain.");
  if (!parent->RenameChild(temporary, *parent, name, replace_existing))
    return Failure(StorageError::kRenameFailed, "Journal publication failed; effects require reconciliation.");
  if (!parent->Sync())
    return Failure(StorageError::kWriteFailed, "Journal directory sync failed; new journal may already be visible.");
  return Result<void>::Success();
}

Result<void> Storage::RemoveDurable(const std::string& relative_path) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) return Failure(resolved.error(), "Invalid journal removal path.");
  auto parent = Parent(root_, relative_path);
  if (!parent) return Failure(StorageError::kOpenFailed, "Private journal directory is unavailable.");
  const auto name = std::filesystem::path(relative_path).filename().string();
  struct stat existing{};
  if (!parent->InspectChild(name, &existing)) {
    if (errno != ENOENT)
      return Failure(StorageError::kRemoveFailed, "Journal removal destination is unsafe or unavailable.");
  } else if (!parent->RemoveRegularFile(name)) {
    return Failure(StorageError::kRemoveFailed, "Owned journal removal failed; effects require reconciliation.");
  }
  if (!parent->Sync())
    return Failure(StorageError::kSyncFailed, "Journal removal sync failed; absence requires reconciliation.");
  return Result<void>::Success();
}
}  // namespace z3cloudplay
