#include "platform/ps5/native_access_posix.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace z3cloudplay::ps5;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
int Initialize(void* data) { ++*static_cast<int*>(data); return 0; }
void Write(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out << bytes;
  CHECK(out.good());
}
}

int main() {
  char name[] = "/tmp/z3cloudplay-native-posix-XXXXXX";
  CHECK(mkdtemp(name));
  const fs::path root = fs::canonical(name);
  const fs::path title = root / "FAKE77031";
  fs::create_directory(title);
  const std::string identity = "z3cloudplay-native/1\nFAKE77031\n" + std::string(64, 'a') + "\n";
  Write(title / "z3cloudplay-package.identity", identity);
  int calls = 0;
  NativeAccessPosixBackend backend("FAKE77031", {&calls, Initialize, Initialize, Initialize});
  const auto ops = backend.Ops();
  CHECK(ops.initialize_user_service(ops.context) == 0);
  CHECK(ops.prepare_graphics_access(ops.context) == 0);
  CHECK(ops.request_data_access(ops.context) == 0);
  CHECK(calls == 3);
  std::string observed;
  CHECK(ops.read_source_identity(ops.context, title.c_str(), &observed));
  CHECK(observed == identity);
  CHECK(ops.path_kind(ops.context, title.c_str()) == NativePathKind::kDirectory);
  CHECK(ops.path_kind(ops.context, (title / "missing").c_str()) == NativePathKind::kMissing);
  const fs::path reserved_probe = title / (".z3cloudplay-access-" + std::to_string(getpid()) + "-0");
  Write(reserved_probe, "preserve foreign probe");
  CHECK(ops.path_accessible(ops.context, title.c_str(), NativePathAccess::kReadWrite));
  std::ifstream probe_input(reserved_probe);
  std::string probe_contents;
  std::getline(probe_input, probe_contents);
  CHECK(probe_contents == "preserve foreign probe");
  probe_input.close();
  for (const auto& entry : fs::directory_iterator(title))
    CHECK(entry.path() == reserved_probe || entry.path() == title / "z3cloudplay-package.identity");
  fs::remove(reserved_probe);

  Write(title / "z3cloudplay-package.identity", "z3cloudplay-native/1\nFAKE77032\n" + std::string(64, 'a') + "\n");
  CHECK(!ops.read_source_identity(ops.context, title.c_str(), &observed));
  Write(title / "z3cloudplay-package.identity", identity + "extra");
  CHECK(!ops.read_source_identity(ops.context, title.c_str(), &observed));
  Write(title / "z3cloudplay-package.identity", "z3cloudplay-native/1\nFAKE77031\n" + std::string(64, 'G') + "\n");
  CHECK(!ops.read_source_identity(ops.context, title.c_str(), &observed));

  Write(root / "identity", identity);
  fs::remove(title / "z3cloudplay-package.identity");
  fs::create_symlink(root / "identity", title / "z3cloudplay-package.identity");
  CHECK(!ops.read_source_identity(ops.context, title.c_str(), &observed));
  fs::create_directory_symlink(title, root / "alias");
  CHECK(ops.path_kind(ops.context, (root / "alias").c_str()) == NativePathKind::kOther);
  CHECK(!ops.path_accessible(ops.context, (root / "alias/identity").c_str(), NativePathAccess::kRead));
  CHECK(ops.path_kind(ops.context, (root / "alias/missing").c_str()) == NativePathKind::kOther);
  CHECK(ops.path_kind(ops.context, (title / "../FAKE77031").c_str()) == NativePathKind::kOther);
  CHECK(ops.path_kind(ops.context, (root / "identity/missing").c_str()) == NativePathKind::kOther);
  CHECK(mkfifo((root / "fifo").c_str(), 0600) == 0);
  CHECK(ops.path_kind(ops.context, (root / "fifo").c_str()) == NativePathKind::kOther);
  CHECK(!ops.path_accessible(ops.context, (root / "fifo").c_str(), NativePathAccess::kRead));

  NativeAccessPosixBackend unavailable("FAKE77031", {});
  const auto missing_ops = unavailable.Ops();
  CHECK(missing_ops.request_data_access(missing_ops.context) != 0);
  fs::remove_all(root);
  std::puts("native_access_posix_test passed");
}
