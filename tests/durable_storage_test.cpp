#include "platform/storage.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using z3cloudplay::Bytes;
using z3cloudplay::Storage;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (false)

namespace {
std::vector<char> barriers;
char fail_sync = 0;
struct Directory {
  Directory() {
    char name[] = "/tmp/z3cloudplay-durable-storage-XXXXXX";
    CHECK(mkdtemp(name));
    path = fs::canonical(name);
    CHECK(chmod(path.c_str(), 0700) == 0);
  }
  ~Directory() { fs::remove_all(path); }
  fs::path path;
};
std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
void Reset() { barriers.clear(); fail_sync = 0; }
void ExclusiveHistoryPreservesExistingAndSyncsBeforeSuccess() {
  Directory directory;
  Storage storage(directory.path.string());
  Reset();
  CHECK(storage.WriteDurableExclusive("history.v1", Bytes{'a'}).ok());
  CHECK(barriers == std::vector<char>({'f', 'd'}));
  struct stat info{};
  CHECK(lstat((directory.path / "history.v1").c_str(), &info) == 0);
  CHECK(info.st_uid == geteuid() && (info.st_mode & 07777) == 0600);
  CHECK(!storage.WriteDurableExclusive("history.v1", Bytes{'b'}).ok());
  CHECK(Read(directory.path / "history.v1") == "a");
  Reset(); fail_sync = 'f';
  CHECK(!storage.WriteDurableExclusive("failed-file.v1", Bytes{'x'}).ok());
  CHECK(!fs::exists(directory.path / "failed-file.v1"));
  bool preserved_temporary = false;
  for (const auto& item : fs::directory_iterator(directory.path))
    if (item.path().filename().string().find(".failed-file.v1.journal-") == 0)
      preserved_temporary = true;
  CHECK(preserved_temporary);
  Reset(); fail_sync = 'd';
  CHECK(!storage.WriteDurableExclusive("failed-parent.v1", Bytes{'y'}).ok());
  CHECK(Read(directory.path / "failed-parent.v1") == "y");
  CHECK(!storage.ReadDurable("failed-parent.v1").ok());
  Reset();
  CHECK(storage.ReadDurable("failed-parent.v1").ok());
  CHECK(!storage.WriteDurableExclusive("failed-parent.v1", Bytes{'z'}).ok());
  CHECK(Read(directory.path / "failed-parent.v1") == "y");
}
void RemovalIsDurableIncludingRecoveredAbsence() {
  Directory directory;
  Storage storage(directory.path.string());
  CHECK(storage.WriteDurable("pending.v1", Bytes{'p'}).ok());
  Reset();
  CHECK(storage.RemoveDurable("pending.v1").ok());
  CHECK(barriers == std::vector<char>({'d'}));
  CHECK(!fs::exists(directory.path / "pending.v1"));
  Reset(); fail_sync = 'd';
  CHECK(!storage.RemoveDurable("pending.v1").ok());
  Reset();
  CHECK(storage.RemoveDurable("pending.v1").ok());
  CHECK(storage.WriteDurable("pending.v1", Bytes{'p'}).ok());
  Reset(); fail_sync = 'd';
  CHECK(!storage.RemoveDurable("pending.v1").ok());
  CHECK(!fs::exists(directory.path / "pending.v1"));
  Reset();
  const auto recovered = storage.ReadDurable("pending.v1");
  CHECK(!recovered.ok() && recovered.error() == z3cloudplay::StorageError::kNotFound);
  CHECK(barriers == std::vector<char>({'d'}));
}
void UnsafeDestinationsAndRootsPreserveEvidence() {
  Directory directory;
  Storage storage(directory.path.string());
  CHECK(storage.WriteDurable("sentinel", Bytes{'s'}).ok());
  fs::create_symlink(directory.path / "sentinel", directory.path / "alias");
  CHECK(!storage.WriteDurableExclusive("alias", Bytes{'x'}).ok());
  CHECK(!storage.RemoveDurable("alias").ok());
  CHECK(fs::is_symlink(fs::symlink_status(directory.path / "alias")));
  CHECK(Read(directory.path / "sentinel") == "s");
  fs::create_directory(directory.path / "child");
  CHECK(!storage.RemoveDurable("child").ok());
  CHECK(!storage.WriteDurableExclusive("../outside", Bytes{'x'}).ok());
  CHECK(!storage.RemoveDurable("../outside").ok());
  CHECK(chmod(directory.path.c_str(), 0755) == 0);
  CHECK(!storage.RemoveDurable("sentinel").ok());
  CHECK(!storage.WriteDurableExclusive("new", Bytes{'x'}).ok());
  CHECK(Read(directory.path / "sentinel") == "s");
  CHECK(chmod(directory.path.c_str(), 0700) == 0);
}
}

extern "C" int fsync(int fd) {
  struct stat info{};
  if (fstat(fd, &info) != 0) return -1;
  const char kind = S_ISDIR(info.st_mode) ? 'd' : 'f';
  barriers.push_back(kind);
  if (fail_sync == kind) { errno = EIO; return -1; }
  using Function = int (*)(int);
  static auto actual = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "fsync"));
  CHECK(actual);
  return actual(fd);
}

int main() {
  ExclusiveHistoryPreservesExistingAndSyncsBeforeSuccess();
  RemovalIsDurableIncludingRecoveredAbsence();
  UnsafeDestinationsAndRootsPreserveEvidence();
}
