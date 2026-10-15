#include "runtime/private_directories.h"
#include "platform/managed_directory.h"
#include <cerrno>
#include <cstring>
namespace playden::runtime {
namespace fs = std::filesystem;
namespace {
using platform::DirectoryPolicy;
using platform::ManagedDirectory;
bool SafeRelative(const fs::path& path) {
  if (path.empty() || path.is_absolute()) return false;
  for (const auto& part : path)
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}
}
bool IsPrivateDirectory(const fs::path& directory) {
  return ManagedDirectory::Open(directory, DirectoryPolicy::kPrivate).has_value();
}
bool IsPrivateDirectoryTree(const fs::path& private_root, const fs::path& directory) {
  const auto relative = directory.lexically_relative(private_root);
  if (directory != private_root && !SafeRelative(relative)) return false;
  auto current = ManagedDirectory::Open(private_root, DirectoryPolicy::kPrivate);
  if (!current) return false;
  if (directory != private_root) {
    for (const auto& part : relative) {
      auto next = current->OpenChild(part.string(), DirectoryPolicy::kPrivate);
      if (!next) return false;
      current = std::move(next);
    }
  }
  return current->Validate();
}
bool EnsurePrivateDirectories(const fs::path& boundary, const fs::path& private_root,
                              const fs::path& directory, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  const auto relative = directory.lexically_relative(boundary);
  const auto private_relative = private_root.lexically_relative(boundary);
  const auto within = directory.lexically_relative(private_root);
  if (!SafeRelative(relative) || !SafeRelative(private_relative) ||
      (directory != private_root && !SafeRelative(within)))
    return fail("private directory is outside its trusted boundary");
  auto current = ManagedDirectory::Open(boundary, DirectoryPolicy::kApprovedTree);
  if (!current) return fail("cannot open directory boundary: " + std::string(strerror(errno)));
  fs::path path = boundary;
  bool in_private = false;
  for (const auto& part : relative) {
    path /= part;
    if (path == private_root) in_private = true;
    auto next = current->OpenChild(part.string(), in_private ? DirectoryPolicy::kPrivate
                                                           : DirectoryPolicy::kApprovedTree, true);
    if (!next)
      return fail("cannot open/create private directory (wrong owner or permissions): " + path.string() + ": " + strerror(errno));
    // Flush the directory and its parent entry before journaled preparation.
    // Failure may follow creation; callers retain their retry/recovery record.
    if (!next->Sync() || !current->Sync())
      return fail("cannot sync private directory: " + path.string() + ": " + strerror(errno));
    current = std::move(next);
  }
  return current->Validate();
}
}
