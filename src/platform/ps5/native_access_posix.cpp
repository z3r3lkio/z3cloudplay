#include "platform/ps5/native_access_posix.h"

#include <cerrno>
#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace z3cloudplay::ps5 {
namespace {
NativePathKind SafeKind(const char* path) {
  if (!path || path[0] != '/') return NativePathKind::kOther;
  const std::string value(path);
  if (value.size() > 4096 || value.size() < 2 || value.back() == '/')
    return NativePathKind::kOther;
  std::size_t cursor = 1;
  bool missing = false;
  struct stat info{};
  while (cursor < value.size()) {
    const auto end = value.find('/', cursor);
    const std::string component = value.substr(cursor, end - cursor);
    if (component.empty() || component == "." || component == "..") {
      return NativePathKind::kOther;
    }
    if (!missing) {
      const std::string prefix = value.substr(0, end);
      const int child = open(prefix.c_str(),
          O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
          (end != std::string::npos ? O_DIRECTORY : 0));
      if (child < 0) {
        if (errno != ENOENT) return NativePathKind::kOther;
        missing = true;
      } else {
        const int result = fstat(child, &info);
        close(child);
        if (result != 0 ||
            (end != std::string::npos && !S_ISDIR(info.st_mode))) {
          return NativePathKind::kOther;
        }
      }
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
  }
  if (missing) return NativePathKind::kMissing;
  if (S_ISDIR(info.st_mode)) return NativePathKind::kDirectory;
  return S_ISREG(info.st_mode) ? NativePathKind::kFile : NativePathKind::kOther;
}

int Invoke(void* context, int (*operation)(void*)) {
  return operation ? operation(context) : ENOSYS;
}
}

NativeAccessPosixBackend::NativeAccessPosixBackend(
    std::string title_id, NativeAccessPreparation preparation)
    : title_id_(std::move(title_id)), preparation_(preparation) {}

NativeAccessOps NativeAccessPosixBackend::Ops() {
  return {this, Initialize, PrepareGraphics, RequestData, Kind, Accessible, Identity};
}

int NativeAccessPosixBackend::Initialize(void* context) {
  const auto& ops = static_cast<NativeAccessPosixBackend*>(context)->preparation_;
  return Invoke(ops.context, ops.initialize_user_service);
}
int NativeAccessPosixBackend::PrepareGraphics(void* context) {
  const auto& ops = static_cast<NativeAccessPosixBackend*>(context)->preparation_;
  return Invoke(ops.context, ops.prepare_graphics_access);
}
int NativeAccessPosixBackend::RequestData(void* context) {
  const auto& ops = static_cast<NativeAccessPosixBackend*>(context)->preparation_;
  return Invoke(ops.context, ops.request_data_access);
}
NativePathKind NativeAccessPosixBackend::Kind(void*, const char* path) {
  return SafeKind(path);
}
bool NativeAccessPosixBackend::Accessible(void*, const char* path, NativePathAccess mode) {
  const auto kind = SafeKind(path);
  if (kind != NativePathKind::kDirectory && kind != NativePathKind::kFile) return false;
  // PS5's sandbox rejects access(2) even when ordinary opens and writes work.
  // Test the actual operation instead of substituting file-mode assumptions.
  const int fd = open(path, (kind == NativePathKind::kFile && mode == NativePathAccess::kReadWrite
                            ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat info{};
  const bool valid = fstat(fd, &info) == 0 &&
      (kind == NativePathKind::kDirectory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
  if (close(fd) != 0 || !valid) return false;
  if (kind != NativePathKind::kDirectory || mode != NativePathAccess::kReadWrite) return true;
  static std::atomic<unsigned long long> sequence{0};
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const std::string probe = std::string(path) + "/.z3cloudplay-access-" +
        std::to_string(getpid()) + "-" + std::to_string(sequence.fetch_add(1));
    const int writable = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (writable < 0) {
      if (errno == EEXIST) continue;
      return false;
    }
    const bool closed = close(writable) == 0;
    const bool removed = unlink(probe.c_str()) == 0;
    return closed && removed;
  }
  return false;
}
bool NativeAccessPosixBackend::Identity(void* context, const char* root,
                                       std::string* output) {
  if (!output || SafeKind(root) != NativePathKind::kDirectory) return false;
  const std::string path = std::string(root) + "/z3cloudplay-package.identity";
  if (SafeKind(path.c_str()) != NativePathKind::kFile) return false;
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info{};
  char bytes[128];
  bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
               info.st_size > 0 && info.st_size < static_cast<off_t>(sizeof(bytes));
  std::size_t count = 0;
  while (valid && count < static_cast<std::size_t>(info.st_size)) {
    const auto got = read(fd, bytes + count, static_cast<std::size_t>(info.st_size) - count);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) { valid = false; break; }
    count += static_cast<std::size_t>(got);
  }
  if (close(fd) != 0) valid = false;
  if (!valid) return false;
  const std::string value(bytes, count);
  const std::string header = "z3cloudplay-native/1\n" +
      static_cast<NativeAccessPosixBackend*>(context)->title_id_ + "\n";
  if (value.size() != header.size() + 65 || value.compare(0, header.size(), header) != 0 ||
      value.back() != '\n') return false;
  for (std::size_t i = header.size(); i < value.size() - 1; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
      return false;
  *output = value;
  return true;
}
}  // namespace z3cloudplay::ps5
