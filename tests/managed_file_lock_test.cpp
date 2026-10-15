#ifdef flock
#undef flock
#endif
#include "platform/managed_file_lock.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s (errno %d)\n", __LINE__, #x, errno); std::exit(1); } } while (0)
namespace fs = std::filesystem;
using namespace z3cloudplay::platform;

namespace {
enum class Fault { kNone, kModeDrift, kFileReplacement, kDirectoryReplacement };
Fault fault = Fault::kNone;
fs::path fault_root;
bool native_unavailable = false;
}

extern "C" int z3cloudplay_test_flock(int fd, int operation) {
  const int result = ::flock(fd, operation);
  if (result != 0) return result;
  const auto apply = fault;
  fault = Fault::kNone;
  if (apply == Fault::kModeDrift) {
    CHECK(::chmod((fault_root / "publication.lock").c_str(), 0644) == 0);
  } else if (apply == Fault::kFileReplacement) {
    fs::rename(fault_root / "publication.lock", fault_root / "old.lock");
    int new_fd = ::open((fault_root / "publication.lock").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(new_fd >= 0 && ::close(new_fd) == 0);
  } else if (apply == Fault::kDirectoryReplacement) {
    fs::rename(fault_root, fault_root.string() + "-old");
    fs::create_directory(fault_root);
    CHECK(::chmod(fault_root.c_str(), 0700) == 0);
  }
  return 0;
}

#if defined(Z3CLOUDPLAY_PLATFORM_PS5) && Z3CLOUDPLAY_PLATFORM_PS5
extern "C" int z3cloudplay_ps5_file_lock(int fd, int operation) {
  if (native_unavailable) { errno = ENOSYS; return -1; }
  return z3cloudplay_test_flock(fd, operation);
}
#endif

namespace {
struct Fixture {
  fs::path root;
  Fixture() {
    char pattern[] = "/tmp/z3cloudplay-managed-lock-XXXXXX";
    CHECK(::mkdtemp(pattern));
    root = fs::canonical(pattern);
    int fd = ::open((root / "publication.lock").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && ::write(fd, "stable", 6) == 6 && ::close(fd) == 0);
  }
  ~Fixture() { fs::remove_all(root); }
};

void ContentionAndMoves(DirectoryBackend backend) {
  Fixture f;
  auto directory = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(directory);
  auto first = ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader);
  auto second = ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader);
  CHECK(first && second);
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
  CHECK(errno == EWOULDBLOCK || errno == EAGAIN);
  auto moved = std::move(*first);
  first.reset();
  second.reset();
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
  auto other_reader = ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader);
  CHECK(other_reader);
  *other_reader = std::move(moved);  // Close its old descriptor; retain moved reader.
  other_reader.reset();
  auto writer = ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher);
  CHECK(writer);
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader));
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
  writer.reset();
  CHECK(ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader));
  char content[7]{};
  int fd = directory->OpenRegularFile("publication.lock", true, 0600);
  CHECK(fd >= 0 && ::read(fd, content, 7) == 6 && std::string(content) == "stable");
  CHECK(::close(fd) == 0);
}

void Admission(DirectoryBackend backend) {
  Fixture f;
  auto directory = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(directory);
  for (const std::string& name : {std::string("missing"), std::string("../publication.lock"),
       std::string(""), std::string("publication.lock\0other", 22)}) {
    CHECK(!ManagedFileLock::Acquire(*directory, name, LockMode::kReader));
  }
  CHECK(!fs::exists(f.root / "missing"));
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", static_cast<LockMode>(-1)));
  CHECK(::chmod((f.root / "publication.lock").c_str(), 0644) == 0);
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader));
  CHECK(::chmod((f.root / "publication.lock").c_str(), 0600) == 0);
  fs::create_symlink("publication.lock", f.root / "link");
  CHECK(!ManagedFileLock::Acquire(*directory, "link", LockMode::kReader));
  CHECK(::link((f.root / "publication.lock").c_str(), (f.root / "alias").c_str()) == 0);
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader));
  CHECK(::unlink((f.root / "alias").c_str()) == 0);
  CHECK(::mkfifo((f.root / "fifo").c_str(), 0600) == 0);
  CHECK(!ManagedFileLock::Acquire(*directory, "fifo", LockMode::kReader));
  auto approved = ManagedDirectory::Open(f.root, DirectoryPolicy::kApprovedTree, backend);
  CHECK(approved && !ManagedFileLock::Acquire(*approved, "publication.lock", LockMode::kReader));
  fs::rename(f.root, f.root.string() + "-old");
  fs::create_directory(f.root);
  CHECK(::chmod(f.root.c_str(), 0700) == 0);
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader));
  CHECK(fs::is_empty(f.root));
  fs::remove_all(f.root.string() + "-old");
}

void ProcessExitReleases(DirectoryBackend backend) {
  Fixture f;
  auto directory = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(directory);
  int ready[2], release[2];
  CHECK(::pipe(ready) == 0 && ::pipe(release) == 0);
  pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(ready[0]); ::close(release[1]);
    auto lease = ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kReader);
    if (!lease || ::write(ready[1], "r", 1) != 1) ::_exit(2);
    char signal;
    if (::read(release[0], &signal, 1) != 1) ::_exit(3);
    ::_exit(0);  // Kernel releases lease; deliberately skip C++ destructors.
  }
  ::close(ready[1]); ::close(release[0]);
  char signal;
  CHECK(::read(ready[0], &signal, 1) == 1);
  CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
  CHECK(::write(release[1], "x", 1) == 1);
  int status;
  CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
  ::close(ready[0]); ::close(release[1]);
}

void AcquisitionDrift(DirectoryBackend backend) {
  for (auto change : {Fault::kModeDrift, Fault::kFileReplacement, Fault::kDirectoryReplacement}) {
    Fixture f;
    auto directory = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
    CHECK(directory);
    fault_root = f.root;
    fault = change;
    CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
    CHECK(fault == Fault::kNone);
    // Refused admission closed the descriptor and released the acquired lease.
    const auto original = change == Fault::kFileReplacement ? f.root / "old.lock" :
        change == Fault::kDirectoryReplacement ? fs::path(f.root.string() + "-old") / "publication.lock" :
        f.root / "publication.lock";
    int fd = ::open(original.c_str(), O_RDONLY);
    CHECK(fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0 && ::close(fd) == 0);
    if (change == Fault::kDirectoryReplacement) fs::remove_all(f.root.string() + "-old");
  }
}
}

int main() {
  ::alarm(15);
#if defined(Z3CLOUDPLAY_PLATFORM_PS5) && Z3CLOUDPLAY_PLATFORM_PS5
  const std::vector<DirectoryBackend> backends{DirectoryBackend::kAbsolutePath};
  {
    Fixture f;
    auto directory = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate);
    CHECK(directory);
    native_unavailable = true;
    CHECK(!ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
    CHECK(errno == ENOSYS);
    native_unavailable = false;
    CHECK(ManagedFileLock::Acquire(*directory, "publication.lock", LockMode::kPublisher));
  }
#else
  const std::vector<DirectoryBackend> backends{DirectoryBackend::kDescriptorRelative, DirectoryBackend::kAbsolutePath};
#endif
  for (auto backend : backends) {
    ContentionAndMoves(backend);
    Admission(backend);
    ProcessExitReleases(backend);
    AcquisitionDrift(backend);
  }
  std::puts("managed lock: private admission, contention, moves and process-exit release pass");
}
