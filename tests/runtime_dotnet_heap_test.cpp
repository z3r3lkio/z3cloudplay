#include "runtime/dotnet_heap.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace z3cloudplay::runtime;

namespace {
void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("z3cloudplay-dotnet-" + std::to_string(::getpid()));
  fs::remove_all(root);
  // Slay the Spire 2's layout: CoreCLR and the runtimeconfig in data_<game>_windows_x86_64.
  const auto data = root / "data_sts2_windows_x86_64";
  const std::string shipped =
      "{\n  \"runtimeOptions\": {\n    \"tfm\": \"net9.0\",\n    \"configProperties\": {\n"
      "      \"System.Reflection.Metadata.MetadataUpdater.IsSupported\": false\n    }\n  }\n}";
  Write(data / "coreclr.dll", "MZ");
  Write(data / "sts2.runtimeconfig.json", shipped);
  // Without configProperties, and an empty one: both get the property.
  Write(root / "tool/CoreCLR.DLL", "MZ");
  Write(root / "tool/tool.runtimeconfig.json", "{\"runtimeOptions\":{\"tfm\":\"net8.0\"}}");
  Write(root / "empty/coreclr.dll", "MZ");
  Write(root / "empty/empty.runtimeconfig.json", "{\"runtimeOptions\":{\"configProperties\":{}}}");
  // A limit the game chose itself is kept; a runtimeconfig without CoreCLR beside it is not ours.
  Write(root / "own/coreclr.dll", "MZ");
  Write(root / "own/own.runtimeconfig.json",
        "{\"runtimeOptions\":{\"configProperties\":{\"System.GC.HeapHardLimit\":1048576}}}");
  Write(root / "framework/app.runtimeconfig.json", "{\"runtimeOptions\":{\"tfm\":\"net8.0\"}}");

  auto result = LimitDotnetHeap(root.string());
  CHECK(result.ok && result.found.size() == 4 && result.changed.size() == 3);
  const auto sts2 = Read(data / "sts2.runtimeconfig.json");
  CHECK(sts2.find("\"System.GC.HeapHardLimit\": 2147483648,") != std::string::npos);
  CHECK(sts2.find("\"System.Reflection.Metadata.MetadataUpdater.IsSupported\": false") != std::string::npos);
  CHECK(Read(root / "tool/tool.runtimeconfig.json") ==
        "{\"runtimeOptions\":{\n    \"configProperties\": {\n      \"System.GC.HeapHardLimit\": 2147483648\n    },"
        "\"tfm\":\"net8.0\"}}");
  CHECK(Read(root / "empty/empty.runtimeconfig.json") ==
        "{\"runtimeOptions\":{\"configProperties\":{\n      \"System.GC.HeapHardLimit\": 2147483648}}}");
  CHECK(Read(root / "own/own.runtimeconfig.json").find("1048576") != std::string::npos);
  CHECK(Read(root / "framework/app.runtimeconfig.json") == "{\"runtimeOptions\":{\"tfm\":\"net8.0\"}}");

  // Idempotent.
  result = LimitDotnetHeap(root.string());
  CHECK(result.ok && result.found.size() == 4 && result.changed.empty());
  // A game without .NET finds nothing.
  CHECK(LimitDotnetHeap((root / "missing").string()).found.empty());

  fs::remove_all(root);
  std::puts("dotnet heap tests passed");
}
