// The root-owned allow-list writer, with the app user standing in for root.
#include "platform/ps5/title_allow_list.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using namespace z3cloudplay::ps5;

namespace {
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
mode_t Mode(const fs::path& path) {
  struct stat info{};
  CHECK(lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}
}  // namespace

int main() {
  char name[] = "/tmp/z3cloudplay-title-allow-list-XXXXXX";
  CHECK(mkdtemp(name));
  const fs::path root = fs::canonical(name);
  const fs::path directory = root / "title-rule";
  int raised = 0, lowered = 0;
  bool refuse = false;
  RootFileOps ops;
  ops.raise = [&] { if (refuse) return false; ++raised; return true; };
  ops.lower = [&] { ++lowered; };
  ops.owner = getuid();
  std::string error;

  // Creates the directory and the list, private to the owner; root is raised only in short
  // steps and always lowered again.
  CHECK(WriteRootOwnedFile(directory, "allow-list", "FAKE78001\nFAKE78011\n", ops, &error));
  CHECK(Read(directory / "allow-list") == "FAKE78001\nFAKE78011\n");
  CHECK(Mode(directory) == 0755 && Mode(directory / "allow-list") == 0644);
  CHECK(raised == 2 && lowered == 2);
  CHECK(!fs::exists(directory / ".allow-list.new"));

  // Replaces it whole, also when empty, and repairs a directory opened up to others.
  CHECK(chmod(directory.c_str(), 0777) == 0);
  CHECK(WriteRootOwnedFile(directory, "allow-list", "", ops, &error));
  CHECK(Read(directory / "allow-list").empty() && Mode(directory) == 0755);

  // A stale temporary file from an interrupted write is replaced.
  { std::ofstream(directory / ".allow-list.new") << "FAKE78999\n"; }
  CHECK(WriteRootOwnedFile(directory, "allow-list", "FAKE78002\n", ops, &error));
  CHECK(Read(directory / "allow-list") == "FAKE78002\n");

  // A directory owned by someone else is refused and left alone.
  ops.owner = getuid() + 1;
  CHECK(!WriteRootOwnedFile(directory, "allow-list", "FAKE78003\n", ops, &error));
  CHECK(Read(directory / "allow-list") == "FAKE78002\n");
  ops.owner = getuid();

  // A symbolic link in the list's place is replaced by a real file, never followed.
  const fs::path elsewhere = root / "elsewhere";
  { std::ofstream(elsewhere) << "untouched\n"; }
  CHECK(unlink((directory / "allow-list").c_str()) == 0);
  CHECK(symlink(elsewhere.c_str(), (directory / "allow-list").c_str()) == 0);
  CHECK(WriteRootOwnedFile(directory, "allow-list", "FAKE78004\n", ops, &error));
  CHECK(Read(elsewhere) == "untouched\n" && !fs::is_symlink(directory / "allow-list"));

  // Without root nothing changes.
  refuse = true;
  CHECK(!WriteRootOwnedFile(directory, "allow-list", "FAKE78005\n", ops, &error) && !error.empty());
  CHECK(Read(directory / "allow-list") == "FAKE78004\n");
  refuse = false;

  // Names stay inside the directory.
  CHECK(!WriteRootOwnedFile(directory, "../escape", "x\n", ops, &error));
  CHECK(!WriteRootOwnedFile(directory, "", "x\n", ops, &error));

  fs::remove_all(root);
  std::puts("title allow-list: private replace, repair, refusal and link safety covered");
  return 0;
}
