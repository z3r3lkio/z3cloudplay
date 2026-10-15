#include "platform/storage.h"
#include "steam/session_store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int failures = 0;
void Check(bool value, const char* message) { if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; } }

class TemporaryDirectory {
 public:
  TemporaryDirectory() { char value[] = "/tmp/z3cloudplay-session-XXXXXX"; if (const char* path = mkdtemp(value)) path_ = path; }
  ~TemporaryDirectory() {
    std::remove((path_ + "/session.v1.tmp").c_str());
    std::remove((path_ + "/session.v1").c_str());
    std::remove((path_ + "/not-a-directory").c_str());
    rmdir(path_.c_str());
  }
  const std::string& path() const { return path_; }
 private: std::string path_;
};

z3cloudplay::StoredSession Example() {
  return {76561198012345678ULL, "synthetic-account", "refresh-SYNTHETIC-SECRET",
          "access-SYNTHETIC-SECRET", 42u};
}

std::uint32_t Checksum(const z3cloudplay::Bytes& bytes, std::size_t start) {
  std::uint32_t value = 2166136261u;
  for (std::size_t i = start; i < bytes.size(); ++i) { value ^= bytes[i]; value *= 16777619u; }
  return value;
}
void Set32(z3cloudplay::Bytes& bytes, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
}

void RoundTripAndPermissions() {
  TemporaryDirectory directory; z3cloudplay::Storage storage(directory.path()); z3cloudplay::SessionStore store(storage);
  const auto expected = Example();
  { std::ofstream stale(directory.path() + "/session.v1.tmp"); stale << "old-secret"; }
  chmod((directory.path() + "/session.v1.tmp").c_str(), 0666);
  Check(store.Save(expected).ok(), "session saves");
  struct stat info{};
  Check(stat((directory.path() + "/session.v1").c_str(), &info) == 0, "session file exists");
  Check((info.st_mode & 0777) == 0600, "session file is private on POSIX");
  const auto loaded = store.Load();
  Check(loaded.ok() && loaded.value().has_value(), "saved session loads");
  if (loaded.ok() && loaded.value()) {
    Check(loaded.value()->steam_id == expected.steam_id, "steam id round trips");
    Check(loaded.value()->account_name == expected.account_name, "account name round trips");
    Check(loaded.value()->refresh_token == expected.refresh_token, "refresh token round trips");
    Check(loaded.value()->access_token == expected.access_token, "access token round trips");
    Check(loaded.value()->cell_id == expected.cell_id, "cell id round trips");
  }
  auto without_cell = expected; without_cell.cell_id.reset(); without_cell.access_token = "replacement-access";
  Check(store.Save(without_cell).ok(), "atomic replacement without optional cell saves");
  const auto replaced = store.Load();
  Check(replaced.ok() && replaced.value() && !replaced.value()->cell_id &&
            replaced.value()->access_token == "replacement-access",
        "replacement and absent optional cell round trip");
}

void MissingAndClear() {
  TemporaryDirectory directory; z3cloudplay::Storage storage(directory.path()); z3cloudplay::SessionStore store(storage);
  auto missing = store.Load();
  Check(missing.ok() && !missing.value(), "missing session is successful empty state");
  Check(store.Save(Example()).ok(), "clear fixture saves");
  { std::ofstream stale(directory.path() + "/session.v1.tmp"); stale << "stale-secret"; }
  Check(store.Clear().ok(), "clear succeeds");
  Check(access((directory.path() + "/session.v1.tmp").c_str(), F_OK) != 0, "clear removes stale temporary secret file");
  auto cleared = store.Load();
  Check(cleared.ok() && !cleared.value(), "cleared session cannot load again");
  Check(store.Clear().ok(), "clear is idempotent when already absent");
}

void RejectsCorruptAndTruncatedData() {
  TemporaryDirectory directory; z3cloudplay::Storage storage(directory.path()); z3cloudplay::SessionStore store(storage);
  Check(store.Save(Example()).ok(), "corruption fixture saves");
  auto bytes = storage.Read("session.v1"); Check(bytes.ok(), "corruption fixture reads");
  if (!bytes.ok()) return;
  auto corrupt = bytes.value(); corrupt[corrupt.size() / 2] ^= 0x5a;
  Check(storage.WriteAtomicPrivate("session.v1", corrupt).ok(), "corrupt fixture writes");
  auto rejected = store.Load();
  Check(!rejected.ok() && rejected.error() == z3cloudplay::StorageError::kInvalidData, "checksum corruption is rejected");
  Check(rejected.message().find("SYNTHETIC-SECRET") == std::string::npos, "corruption error does not expose tokens");
  corrupt.resize(7); Check(storage.WriteAtomicPrivate("session.v1", corrupt).ok(), "truncated fixture writes");
  rejected = store.Load();
  Check(!rejected.ok() && rejected.error() == z3cloudplay::StorageError::kInvalidData, "truncated session is rejected");
  const z3cloudplay::Bytes oversized(64 * 1024 + 1, 0x5a);
  Check(storage.WriteAtomicPrivate("session.v1", oversized).ok(), "oversized disk fixture writes");
  rejected = store.Load();
  Check(!rejected.ok(), "oversized on-disk session is rejected by the read bound");
}

void RejectsOversizeAndSanitizesWriteFailure() {
  TemporaryDirectory directory; z3cloudplay::Storage storage(directory.path()); z3cloudplay::SessionStore store(storage);
  auto oversized = Example(); oversized.refresh_token.assign(70000, 's');
  auto result = store.Save(oversized);
  Check(!result.ok() && result.error() == z3cloudplay::StorageError::kInvalidData, "oversized token is rejected");
  Check(result.message().find("synthetic-account") == std::string::npos, "validation error omits account name");
  auto invalid = Example(); invalid.steam_id = 0;
  Check(!store.Save(invalid).ok(), "zero Steam identity is not persisted");
  invalid = Example(); invalid.refresh_token.clear();
  Check(!store.Save(invalid).ok(), "empty refresh credential is not persisted");
  const std::string blocker = directory.path() + "/not-a-directory";
  { std::ofstream file(blocker); file << "block"; }
  z3cloudplay::Storage failing(blocker); z3cloudplay::SessionStore failing_store(failing);
  result = failing_store.Save(Example());
  Check(!result.ok(), "write failure propagates");
  Check(result.message().find("SYNTHETIC-SECRET") == std::string::npos, "write error omits tokens");
  result = failing_store.Clear();
  Check(!result.ok(), "clear failure propagates instead of claiming success");
}

void RejectsSemanticallyInvalidLoadedSession() {
  TemporaryDirectory directory; z3cloudplay::Storage storage(directory.path()); z3cloudplay::SessionStore store(storage);
  Check(store.Save(Example()).ok(), "semantic corruption fixture saves");
  auto read = storage.Read("session.v1"); Check(read.ok(), "semantic fixture reads");
  if (!read.ok()) return;
  auto bytes = read.value();
  for (std::size_t index = 20; index < 28; ++index) bytes[index] = 0;
  Set32(bytes, 16, Checksum(bytes, 20));
  Check(storage.WriteAtomicPrivate("session.v1", bytes).ok(), "semantic fixture writes");
  const auto loaded = store.Load();
  Check(!loaded.ok() && loaded.error() == z3cloudplay::StorageError::kInvalidData,
        "valid envelope with zero Steam identity is rejected on load");
}
}

int main() {
  RoundTripAndPermissions(); MissingAndClear(); RejectsCorruptAndTruncatedData();
  RejectsOversizeAndSanitizesWriteFailure(); RejectsSemanticallyInvalidLoadedSession();
  if (failures) { std::fprintf(stderr, "%d session store assertion(s) failed\n", failures); return EXIT_FAILURE; }
  std::puts("session_store_test: all assertions passed"); return EXIT_SUCCESS;
}
