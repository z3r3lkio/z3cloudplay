// title_allow_list.cpp - The root-owned list of game titles an any-title kit's helper serves.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include "platform/ps5/title_allow_list.h"

#include "runtime/package_installer.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace z3cloudplay::ps5 {
namespace {
bool Fail(std::string* error, const std::string& what, int code) {
  if (error) *error = what + " (" + std::strerror(code) + ")";
  return false;
}

// Runs one root step: raises, runs it, always lowers again. Returns the step's errno, 0 for success.
template <typename Step> int AsRoot(const RootFileOps& ops, Step step) {
  if (!ops.raise()) return EPERM;
  errno = 0;
  const int result = step() ? 0 : (errno ? errno : EIO);
  ops.lower();
  return result;
}

bool Private(const struct stat& info, uid_t owner, mode_t mode) {
  return info.st_uid == owner && (info.st_mode & 07777) == mode;
}
}  // namespace

bool WriteRootOwnedFile(const std::filesystem::path& directory, const std::string& name,
                        const std::string& contents, const RootFileOps& ops, std::string* error) {
  if (!ops.raise || !ops.lower || name.empty() || name.find('/') != std::string::npos)
    return Fail(error, "invalid request", EINVAL);
  const auto target = directory / name;
  const auto temporary = directory / ("." + name + ".new");
  int fd = -1;
  int code = AsRoot(ops, [&] {
    if (::mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) return false;
    struct stat info{};
    if (::lstat(directory.c_str(), &info) != 0) return false;
    if (!S_ISDIR(info.st_mode) || info.st_uid != ops.owner) { errno = EPERM; return false; }
    if ((info.st_mode & 07777) != 0755 && ::chmod(directory.c_str(), 0755) != 0) return false;
    if (::unlink(temporary.c_str()) != 0 && errno != ENOENT) return false;
    fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    for (std::size_t done = 0; done < contents.size();) {
      const auto count = ::write(fd, contents.data() + done, contents.size() - done);
      if (count <= 0) return false;
      done += static_cast<std::size_t>(count);
    }
    return ::fchmod(fd, 0644) == 0;
  });
  // Syncing needs no privilege and may be slow, so it runs as the app user.
  if (!code && ::fsync(fd) != 0) code = errno ? errno : EIO;
  if (fd >= 0) ::close(fd);
  if (!code) code = AsRoot(ops, [&] { return ::rename(temporary.c_str(), target.c_str()) == 0; });
  if (code) {
    AsRoot(ops, [&] { return ::unlink(temporary.c_str()) == 0; });
    return Fail(error, "cannot write " + target.string(), code);
  }
  if (const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir >= 0) {
    ::fsync(dir);
    ::close(dir);
  }
  struct stat info{};
  if (::lstat(target.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
      !Private(info, ops.owner, 0644) || static_cast<std::size_t>(info.st_size) != contents.size())
    return Fail(error, target.string() + " is not the private file just written", EPERM);
  if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || !Private(info, ops.owner, 0755))
    return Fail(error, directory.string() + " is not private", EPERM);
  return true;
}

bool WriteTitleAllowList(const std::string& contents, std::string* error) {
  const std::filesystem::path list = runtime::kTitleRuleAllowList;
  const uid_t user = ::geteuid();
  RootFileOps ops;
  ops.raise = [] { return ::seteuid(0) == 0; };
  ops.lower = [user] { (void)::seteuid(user); };
  ops.owner = 0;
  const bool written = WriteRootOwnedFile(list.parent_path(), list.filename().string(), contents, ops, error);
  if (::geteuid() != user) return Fail(error, "the app user could not be restored", EPERM);
  return written;
}
}  // namespace z3cloudplay::ps5
