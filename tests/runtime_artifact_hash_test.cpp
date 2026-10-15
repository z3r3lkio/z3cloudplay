#include "runtime/artifact_hash.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

#ifdef Z3CLOUDPLAY_TEST_AT_UNAVAILABLE
extern "C" int openat(int, const char*, int, ...) { errno = EINVAL; return -1; }
#endif

int main() {
  namespace fs = std::filesystem;
  using namespace z3cloudplay::runtime;
  assert(HashArtifactBytes("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(HashArtifactBytes("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  char temporary[] = "/tmp/z3cloudplay-artifact-hash-XXXXXX";
  assert(mkdtemp(temporary));
  const fs::path root = fs::canonical(temporary);
  fs::create_directory(root / "payload");
  const std::string large(2 * 1024 * 1024 + 17, 'x');
  {
    std::ofstream out(root / "payload/file", std::ios::binary);
    out.write(large.data(), large.size());
    assert(out.good());
  }
  assert(HashArtifactFile(root / "payload/file") == HashArtifactBytes(large));
  assert(!HashArtifactFile(root / "missing"));
  assert(!HashArtifactFile(root / "payload"));
  fs::create_symlink("payload/file", root / "file-link");
  fs::create_directory_symlink("payload", root / "directory-link");
  assert(!HashArtifactFile(root / "file-link"));
  assert(!HashArtifactFile(root / "directory-link/file"));
  assert(!HashArtifactFile(root / "payload/../payload/file"));
  assert(mkfifo((root / "fifo").c_str(), 0600) == 0);
  assert(!HashArtifactFile(root / "fifo"));
  fs::remove_all(root);
}
