#include "runtime/dotnet_heap.h"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kMaxRuntimeConfig = 64 * 1024;
constexpr const char* kLimitKey = "System.GC.HeapHardLimit";

bool Lower(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool IsRegular(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool IsFolder(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool ReadSmall(const fs::path& path, std::string* bytes) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return false;
  bytes->clear();
  char buffer[4096];
  bool ok = true;
  while (ok) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || bytes->size() + static_cast<std::size_t>(count) > kMaxRuntimeConfig) ok = false;
    else if (count == 0) break;
    else bytes->append(buffer, static_cast<std::size_t>(count));
  }
  return ::close(file) == 0 && ok;
}

// Through a temporary file in the same folder, synced, then renamed over the original.
bool WriteAtomic(const fs::path& path, std::string_view bytes) {
  const auto temporary = path.parent_path() / (".playden-" + path.filename().string() + ".tmp");
  ::unlink(temporary.c_str());
  const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (file < 0) return false;
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < bytes.size()) {
    const auto count = ::write(file, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else done += static_cast<std::size_t>(count);
  }
  ok = ::fsync(file) == 0 && ok;
  ok = ::close(file) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}

std::size_t Count(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

// Inserts text right after the '{' that opens the object of the first `"key":` at or after from.
bool InsertIntoObject(std::string* text, std::string_view key, std::size_t from, std::string_view member) {
  const auto quoted = "\"" + std::string(key) + "\"";
  const auto at = text->find(quoted, from);
  if (at == std::string::npos) return false;
  std::size_t brace = at + quoted.size();
  while (brace < text->size() && (*text)[brace] != '{') {
    const char c = (*text)[brace];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ':') return false;
    ++brace;
  }
  if (brace >= text->size()) return false;
  std::size_t next = brace + 1;
  while (next < text->size() && std::string_view(" \t\r\n").find((*text)[next]) != std::string_view::npos) ++next;
  const bool empty = next < text->size() && (*text)[next] == '}';
  text->insert(brace + 1, std::string(member) + (empty ? "" : ","));
  return true;
}

// The runtimeconfig with the limit added, or nothing when its layout is not the plain one the
// .NET SDK writes (one runtimeOptions object, at most one configProperties object in it).
std::optional<std::string> WithLimit(const std::string& original) {
  if (Count(original, "\"runtimeOptions\"") != 1 || Count(original, "\"configProperties\"") > 1) return std::nullopt;
  const auto value = std::to_string(kDotnetHeapHardLimit);
  std::string text = original;
  const auto options = text.find("\"runtimeOptions\"");
  const auto properties = text.find("\"configProperties\"");
  bool inserted;
  if (properties != std::string::npos) {
    if (properties < options) return std::nullopt;
    inserted = InsertIntoObject(&text, "configProperties", options, "\n      \"" + std::string(kLimitKey) + "\": " + value);
  } else {
    inserted = InsertIntoObject(&text, "runtimeOptions", 0,
                                "\n    \"configProperties\": {\n      \"" + std::string(kLimitKey) + "\": " + value + "\n    }");
  }
  if (!inserted) return std::nullopt;
  return text;
}

}  // namespace

DotnetHeapResult LimitDotnetHeap(const std::string& game_root) {
  DotnetHeapResult result;
  const fs::path root(game_root);
  std::vector<fs::path> folders{root};
  std::error_code error;
  for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    if (IsFolder(it->path()) && it->path().filename().string()[0] != '.') folders.push_back(it->path());
  for (const auto& folder : folders) {
    bool coreclr = false;
    std::vector<fs::path> configs;
    std::error_code list_error;
    for (fs::directory_iterator it(folder, list_error), end; !list_error && it != end; it.increment(list_error)) {
      const auto name = it->path().filename().string();
      if (!IsRegular(it->path())) continue;
      if (Lower(name, "coreclr.dll")) coreclr = true;
      const std::string_view suffix = ".runtimeconfig.json";
      if (name.size() > suffix.size() && Lower(std::string_view(name).substr(name.size() - suffix.size()), suffix))
        configs.push_back(it->path());
    }
    if (!coreclr) continue;
    for (const auto& config : configs) {
      const auto relative = config.lexically_relative(root).generic_string();
      result.found.push_back(relative);
      std::string bytes;
      if (!ReadSmall(config, &bytes) || bytes.find("System.GC.HeapHardLimit") != std::string::npos) continue;
      const auto updated = WithLimit(bytes);
      if (!updated) continue;  // an unusual layout is left as the game shipped it
      if (!WriteAtomic(config, *updated)) {
        result.ok = false;
        result.message = "Playden could not update the game's .NET settings (" + relative + ").";
        return result;
      }
      result.changed.push_back(relative);
    }
  }
  return result;
}

}  // namespace playden::runtime
