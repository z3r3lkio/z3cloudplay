#pragma once

#include "platform/managed_directory.h"

namespace z3cloudplay::platform {

enum class LockMode { kReader, kPublisher };

// Cooperative publication lease. The caller creates/syncs the private lock file
// before use and never renames, unlinks or truncates it. Admission checks its
// identity around nonblocking acquisition; this inherits ManagedDirectory's
// stable-namespace assumption, not protection against adversarial replacement.
class ManagedFileLock {
 public:
  static std::optional<ManagedFileLock> Acquire(
      const ManagedDirectory& directory, const std::string& filename, LockMode mode);
  ManagedFileLock(ManagedFileLock&& other) noexcept;
  ManagedFileLock& operator=(ManagedFileLock&& other) noexcept;
  ~ManagedFileLock();
  ManagedFileLock(const ManagedFileLock&) = delete;
  ManagedFileLock& operator=(const ManagedFileLock&) = delete;

  // Borrowed by the locked semantic verifier; callers never close or convert it.
  int fd() const { return descriptor_; }
  // The lease is still held: the descriptor is open and is the file now named
  // filename. Same as Holds(fd(), directory.path() / filename).
  bool Validate(const ManagedDirectory& directory, const std::string& filename) const;
  // Whether descriptor is open on the regular file lock_path names (same device
  // and inode). The lease's only re-check; owner, mode and ancestors are not
  // re-proven (only Z3CloudPlay writes these folders).
  static bool Holds(int descriptor, const std::filesystem::path& lock_path);

 private:
  explicit ManagedFileLock(int descriptor) : descriptor_(descriptor) {}
  int descriptor_ = -1;
};

}  // namespace z3cloudplay::platform
