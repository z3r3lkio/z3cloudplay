#include "platform/storage.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace z3cloudplay {
namespace {

std::string ErrnoMessage(const std::string& action,
                         const std::string& relative_path,
                         int error_number) {
  return action + " '" + relative_path + "': " + std::strerror(error_number);
}

std::string WriteErrorMessage(const std::string& action,
                              const std::string& relative_path,
                              int error_number,
                              bool numeric_only) {
  if (!numeric_only) return ErrnoMessage(action, relative_path, error_number);
  return action + " '" + relative_path + "' (errno " + std::to_string(error_number) + ")";
}

bool IsSafeRelativePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos ||
      (path.size() >= 2 && path[1] == ':')) {
    return false;
  }

  std::size_t segment_start = 0;
  while (segment_start <= path.size()) {
    const std::size_t separator = path.find('/', segment_start);
    const std::size_t segment_end =
        separator == std::string::npos ? path.size() : separator;
    const std::string segment =
        path.substr(segment_start, segment_end - segment_start);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    if (separator == std::string::npos) {
      break;
    }
    segment_start = separator + 1;
  }
  return true;
}

}  // namespace

Storage::Storage(std::string root) : root_(std::move(root)) {
  while (root_.size() > 1 && root_.back() == '/') {
    root_.pop_back();
  }
}

Result<void> Storage::EnsureRootDirectory() const {
  if(root_.empty() || root_.find('\0')!=std::string::npos)
    return Result<void>::Failure(StorageError::kInvalidPath,"Storage root is invalid.");
  if(mkdir(root_.c_str(),0700)!=0 && errno!=EEXIST)
    return Result<void>::Failure(StorageError::kOpenFailed,"Storage root could not be created.");
  struct stat info{};
  if(stat(root_.c_str(),&info)!=0 || !S_ISDIR(info.st_mode))
    return Result<void>::Failure(StorageError::kOpenFailed,"Storage root is unavailable.");
  return Result<void>::Success();
}

Result<std::string> Storage::Resolve(const std::string& relative_path) const {
  if (root_.empty()) {
    return Result<std::string>::Failure(StorageError::kInvalidPath,
                                        "storage root is empty");
  }
  if (!IsSafeRelativePath(relative_path)) {
    return Result<std::string>::Failure(
        StorageError::kInvalidPath,
        "storage path must be a normalized relative path: '" + relative_path + "'");
  }
  return Result<std::string>::Success(root_ + "/" + relative_path);
}

Result<Bytes> Storage::Read(const std::string& relative_path, std::size_t max_bytes) const {
  if (max_bytes == 0 || max_bytes > 32 * 1024 * 1024)
    return Result<Bytes>::Failure(StorageError::kReadFailed, "invalid bounded read size");
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) {
    return Result<Bytes>::Failure(resolved.error(), resolved.message());
  }

  errno = 0;
  std::FILE* file = std::fopen(resolved.value().c_str(), "rb");
  if (file == nullptr) {
    const int open_error = errno;
    const StorageError error =
        open_error == ENOENT ? StorageError::kNotFound : StorageError::kOpenFailed;
    return Result<Bytes>::Failure(
        error, ErrnoMessage("could not open", relative_path, open_error));
  }

  Bytes bytes;
  std::uint8_t buffer[4096];
  while (true) {
    const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file);
    if (count > max_bytes - bytes.size()) {
      std::fclose(file);
      return Result<Bytes>::Failure(
          StorageError::kReadFailed,
          "could not read '" + relative_path +
              "': file exceeds the " + std::to_string(max_bytes) + "-byte limit");
    }
    bytes.insert(bytes.end(), buffer, buffer + count);
    if (count < sizeof(buffer)) {
      if (std::ferror(file) != 0) {
        const int read_error = errno;
        std::fclose(file);
        return Result<Bytes>::Failure(
            StorageError::kReadFailed,
            ErrnoMessage("could not read", relative_path, read_error));
      }
      break;
    }
  }
  std::fclose(file);
  return Result<Bytes>::Success(std::move(bytes));
}

Result<void> Storage::WriteAtomic(const std::string& relative_path,
                                  const Bytes& bytes) const {
  return WriteAtomicImpl(relative_path, bytes, false);
}

Result<void> Storage::WriteAtomicPrivate(const std::string& relative_path,
                                         const Bytes& bytes) const {
  return WriteAtomicImpl(relative_path, bytes, true);
}

Result<void> Storage::WriteAtomicImpl(const std::string& relative_path,
                                      const Bytes& bytes,
                                      bool private_file) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) {
    return Result<void>::Failure(resolved.error(), resolved.message());
  }

  const std::string temporary_path = resolved.value() + ".tmp";
  errno = 0;
  std::FILE* file = nullptr;
  if (private_file) {
    const int descriptor = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
      return Result<void>::Failure(StorageError::kOpenFailed,
                                   "private storage open failed for '" + relative_path +
                                       "' (errno " + std::to_string(errno) + ")");
    }
    if (fchmod(descriptor, 0600) != 0) {
      const int permission_error = errno;
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(StorageError::kPermissionFailed,
                                   "private storage permissions failed for '" + relative_path +
                                       "' (errno " + std::to_string(permission_error) + ")");
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0) {
      const int permission_error = errno;
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(
          StorageError::kPermissionFailed,
          "private storage permission verification failed for '" + relative_path +
              "' (errno " + std::to_string(permission_error) + ")");
    }
    if ((status.st_mode & 0777) != 0600) {
      const unsigned observed_mode = static_cast<unsigned>(status.st_mode & 0777);
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(
          StorageError::kPermissionFailed,
          "private storage mode verification failed for '" + relative_path +
              "' (mode " + std::to_string(observed_mode) + ")");
    }
    file = fdopen(descriptor, "wb");
    if (!file) {
      const int stream_error = errno;
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(StorageError::kOpenFailed,
                                   "private storage stream failed for '" + relative_path +
                                       "' (errno " + std::to_string(stream_error) + ")");
    }
  } else {
    file = std::fopen(temporary_path.c_str(), "wb");
  }
  if (file == nullptr) {
    return Result<void>::Failure(
        StorageError::kOpenFailed,
        WriteErrorMessage("could not open temporary file for", relative_path, errno, private_file));
  }

  if (!bytes.empty() &&
      std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    const int write_error = errno;
    std::fclose(file);
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kWriteFailed,
        WriteErrorMessage("could not write", relative_path, write_error, private_file));
  }
  if (std::fflush(file) != 0) {
    const int flush_error = errno;
    std::fclose(file);
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kWriteFailed,
        WriteErrorMessage("could not flush", relative_path, flush_error, private_file));
  }
  if (std::fclose(file) != 0) {
    const int close_error = errno;
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kWriteFailed,
        WriteErrorMessage("could not close", relative_path, close_error, private_file));
  }

  errno = 0;
  if (std::rename(temporary_path.c_str(), resolved.value().c_str()) != 0) {
    const int rename_error = errno;
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kRenameFailed,
        WriteErrorMessage("could not replace", relative_path, rename_error, private_file));
  }
  return Result<void>::Success();
}

Result<void> Storage::Remove(const std::string& relative_path) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) return Result<void>::Failure(resolved.error(), resolved.message());
  errno = 0;
  if (unlink(resolved.value().c_str()) == 0 || errno == ENOENT) return Result<void>::Success();
  const int remove_error = errno;
  return Result<void>::Failure(StorageError::kRemoveFailed,
                               "storage remove failed for '" + relative_path + "' (errno " +
                                   std::to_string(remove_error) + ")");
}

}  // namespace z3cloudplay
