#include "runtime/private_directories.h"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

// Reproduce the native contract: descriptor-relative openat is unavailable.
extern "C" int openat(int, const char*, int, ...) { errno = EINVAL; return -1; }
extern "C" int mkdirat(int, const char*, mode_t) { errno = EINVAL; return -1; }

int main() {
  namespace fs = std::filesystem;
  char scratch[] = "/tmp/z3cloudplay-private-policy-XXXXXX";
  const char* made = mkdtemp(scratch);
  assert(made);
  const auto boundary = fs::canonical(made);
  const auto root = boundary / "install";
  const auto bottle = root / "library/prefixes/game";
  std::string error;
  assert(z3cloudplay::runtime::EnsurePrivateDirectories(boundary, root, bottle, &error));
  assert(z3cloudplay::runtime::IsPrivateDirectoryTree(root, bottle));
  assert(z3cloudplay::runtime::IsPrivateDirectory(bottle));
  assert(chmod((root / "library").c_str(), 0755) == 0);
  assert(!z3cloudplay::runtime::IsPrivateDirectoryTree(root, bottle));
  assert(!z3cloudplay::runtime::EnsurePrivateDirectories(boundary, root, bottle, &error));
  assert(chmod((root / "library").c_str(), 0700) == 0);
  fs::create_directory_symlink(boundary, root / "escape");
  assert(!z3cloudplay::runtime::EnsurePrivateDirectories(boundary, root, root / "escape/child", &error));
  assert(!fs::exists(boundary / "child"));
  assert(!z3cloudplay::runtime::EnsurePrivateDirectories(boundary, root, root / "../outside", &error));
  fs::remove_all(boundary);
  std::cout << "private directory callers pass with native relative operations unavailable\n";
}
