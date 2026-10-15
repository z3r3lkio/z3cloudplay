#include "library/organization.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;
void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char value[] = "/tmp/z3cloudplay-organization-XXXXXX";
    if (const char* path = mkdtemp(value)) path_ = path;
  }
  ~TemporaryDirectory() {
    if (!path_.empty()) { std::error_code error; std::filesystem::remove_all(path_, error); }
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

constexpr std::uint64_t kFirstAccount = 76561198000000001ULL;
constexpr std::uint64_t kSecondAccount = 76561198000000002ULL;

std::string Path(std::uint64_t account) {
  return "organization-" + std::to_string(account) + ".v1";
}

void RewriteChecksum(z3cloudplay::Bytes& bytes) {
  std::uint32_t checksum = 2166136261u;
  for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
    checksum ^= bytes[index]; checksum *= 16777619u;
  }
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes[bytes.size() - 4 + shift / 8] = static_cast<std::uint8_t>(checksum >> shift);
}

bool ReplaceBytes(z3cloudplay::Bytes& bytes, const std::string& from, const std::string& to) {
  if (from.size() != to.size()) return false;
  const auto found = std::search(bytes.begin(), bytes.end(), from.begin(), from.end());
  if (found == bytes.end()) return false;
  std::copy(to.begin(), to.end(), found);
  RewriteChecksum(bytes);
  return true;
}

z3cloudplay::OrganizationSnapshot Example() {
  z3cloudplay::OrganizationSnapshot result;
  result.favorites = {10, 20};
  result.hidden = {30};
  result.collections = {
      {"collection-1", "旅のゲーム", true, {10, 30}},
      {"collection-2", "Co-op café", false, {20}},
  };
  result.ratings = {
      {10, z3cloudplay::CompatibilityRating::kWorks},
      {20, z3cloudplay::CompatibilityRating::kPlayable},
      {30, z3cloudplay::CompatibilityRating::kBroken},
  };
  result.notes = {{10, "再訪したい"}, {20, "Français — bientôt"}};
  result.query = "星 café";
  result.scope = "collection-1";
  result.sort = "recent-played";
  result.filters = {"compat-playable"};
  result.selected_game_id = 20;
  return result;
}

void RoundTripAndIsolation() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::OrganizationStore store(storage);
  const auto expected = Example();
  Check(store.Save(kFirstAccount, expected).ok(), "organization snapshot saves");
  const auto restored = store.Load(kFirstAccount);
  Check(restored.ok() && restored.value() && *restored.value() == expected,
        "Unicode organization snapshot round trips without loss");
  const auto other = store.Load(kSecondAccount);
  Check(other.ok() && !other.value(), "another account sees a distinct missing snapshot");
  const auto first_bytes = storage.Read(Path(kFirstAccount));
  Check(first_bytes.ok() && storage.WriteAtomicPrivate(Path(kSecondAccount), first_bytes.value()).ok(),
        "copied cross-account fixture writes");
  const auto copied = store.Load(kSecondAccount);
  Check(!copied.ok() && copied.error() == z3cloudplay::StorageError::kInvalidData,
        "embedded account identity rejects a snapshot copied across accounts");
  Check(!store.Save(0, expected).ok() && !store.Load(0).ok() && !store.Clear(0).ok(),
        "zero account identity is rejected");

  struct stat info {};
  Check(stat((directory.path() + "/" + Path(kFirstAccount)).c_str(), &info) == 0,
        "organization file exists");
  Check((info.st_mode & 0777) == 0600, "organization file is owner-only");
}

void RejectsInvalidValuesWithoutReplacingGoodData() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::OrganizationStore store(storage);
  const auto expected = Example();
  Check(store.Save(kFirstAccount, expected).ok(), "baseline organization saves");
  auto rejects = [&](z3cloudplay::OrganizationSnapshot invalid, const char* message) {
    Check(!store.Save(kFirstAccount, invalid).ok(), message);
    const auto retained = store.Load(kFirstAccount);
    Check(retained.ok() && retained.value() && *retained.value() == expected,
          "rejected update preserves previous organization file");
  };

  auto invalid = expected; invalid.schema_version = 2; rejects(invalid, "future schema rejected");
  invalid = expected; invalid.favorites.insert(0); rejects(invalid, "zero favorite rejected");
  invalid = expected; invalid.collections.push_back(invalid.collections.front()); rejects(invalid, "duplicate collection ID rejected");
  invalid = expected; invalid.collections.front().id = "../escape"; rejects(invalid, "unsafe collection ID rejected");
  invalid = expected; invalid.collections.front().id = "installed"; rejects(invalid, "simulation rail ID cannot be a collection ID");
  invalid = expected; invalid.collections.front().id = "new-collection"; rejects(invalid, "collection action ID cannot be a collection ID");
  invalid = expected; invalid.collections.front().name.assign(129, 'n'); rejects(invalid, "oversized collection name rejected");
  invalid = expected; invalid.collections.front().name = "   \t"; rejects(invalid, "whitespace-only collection name rejected");
  invalid = expected; invalid.collections.front().name = " Leading"; rejects(invalid, "leading collection whitespace rejected instead of normalized");
  invalid = expected; invalid.collections.front().name = "Trailing "; rejects(invalid, "trailing collection whitespace rejected instead of normalized");
  invalid = expected; invalid.collections[1].name = invalid.collections[0].name; rejects(invalid, "duplicate collection name rejected");
  invalid = expected; invalid.collections.front().members.insert(0); rejects(invalid, "zero collection member rejected");
  invalid = expected; invalid.ratings[10] = static_cast<z3cloudplay::CompatibilityRating>(4); rejects(invalid, "unknown compatibility rejected");
  invalid = expected; invalid.notes[10].assign(4097, 'n'); rejects(invalid, "oversized note rejected");
  invalid = expected; invalid.query.assign(257, 'q'); rejects(invalid, "oversized query rejected");
  invalid = expected; invalid.sort = "invented"; rejects(invalid, "unknown sort rejected");
  invalid = expected; invalid.filters.insert("fixture-only"); rejects(invalid, "unknown filter rejected");
  {
    // The Library's Installed rail is a live scope, so choosing it is saved and restored.
    auto installed = expected; installed.scope = "installed";
    Check(store.Save(kFirstAccount, installed).ok(), "installed scope accepted");
    const auto restored = store.Load(kFirstAccount);
    Check(restored.ok() && restored.value() && restored.value()->scope == "installed", "installed scope restored");
    Check(store.Save(kFirstAccount, expected).ok(), "restore the expected organization file after installed scope");
  }
  invalid = expected; invalid.scope = "new-collection"; rejects(invalid, "collection action cannot be restored as scope");
  invalid = expected; invalid.sort = "install-size"; rejects(invalid, "simulation-only metadata sort rejected");
  {
    // Acquisition dates come from the Steam CM, so the recently-added sort is a live fact.
    auto dated = expected; dated.sort = "recent-added";
    Check(store.Save(kFirstAccount, dated).ok(), "recently-added sort accepted");
    const auto restored = store.Load(kFirstAccount);
    Check(restored.ok() && restored.value() && restored.value()->sort == "recent-added",
          "recently-added sort restored");
    Check(store.Save(kFirstAccount, expected).ok(), "restore the expected organization file");
  }
  invalid = expected; invalid.filters = {"controller-full"}; rejects(invalid, "simulation-only controller filter rejected");
  invalid = expected; invalid.scope = "missing-collection"; rejects(invalid, "unknown scope rejected");
  invalid = expected; invalid.notes[40] = std::string("bad-") + static_cast<char>(0xc0);
  rejects(invalid, "invalid UTF-8 note rejected");

  invalid = expected;
  invalid.favorites.clear();
  for (z3cloudplay::GameId id = 1; id <= 10001; ++id) invalid.favorites.insert(id);
  rejects(invalid, "more than ten thousand organization game IDs rejected");

  auto changed = expected;
  changed.query = "replacement";
  const std::string temporary_path = directory.path() + "/" + Path(kFirstAccount) + ".tmp";
  Check(mkdir(temporary_path.c_str(), 0700) == 0, "write-failure blocker creates");
  Check(!store.Save(kFirstAccount, changed).ok(), "atomic private write failure propagates");
  const auto retained = store.Load(kFirstAccount);
  Check(retained.ok() && retained.value() && *retained.value() == expected,
        "atomic write failure preserves the previous organization file");
  rmdir(temporary_path.c_str());
}

void RejectsDamagedAndOversizedFiles() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::OrganizationStore store(storage);
  Check(store.Save(kFirstAccount, Example()).ok(), "corruption fixture saves");
  const auto stored = storage.Read(Path(kFirstAccount));
  Check(stored.ok(), "corruption fixture reads");
  if (!stored.ok()) return;

  auto corrupt = stored.value();
  corrupt[corrupt.size() / 2] ^= 0x5a;
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), corrupt).ok(), "corrupt file writes");
  auto loaded = store.Load(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == z3cloudplay::StorageError::kInvalidData,
        "checksum corruption is distinguished from missing data");

  auto truncated = stored.value();
  truncated.resize(9);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), truncated).ok(), "truncated file writes");
  loaded = store.Load(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == z3cloudplay::StorageError::kInvalidData,
        "truncated organization is rejected");

  auto future = stored.value();
  future[8] = 2;
  RewriteChecksum(future);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), future).ok(), "future-version fixture writes");
  loaded = store.Load(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == z3cloudplay::StorageError::kInvalidData,
        "valid-checksum future version is rejected");

  const z3cloudplay::Bytes oversized(1024 * 1024 + 1, 0x41);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), oversized).ok(), "oversized file fixture writes");
  loaded = store.Load(kFirstAccount);
  Check(!loaded.ok(), "oversized organization is rejected by exact read bound");

  const std::string blocker = directory.path() + "/not-a-directory";
  { std::ofstream output(blocker); output << "block"; }
  z3cloudplay::Storage failing_storage(blocker);
  z3cloudplay::OrganizationStore failing(failing_storage);
  loaded = failing.Load(kFirstAccount);
  Check(!loaded.ok() && loaded.error() != z3cloudplay::StorageError::kNotFound,
        "storage read failure remains distinct from missing organization");
  const auto clear_failed = failing.Clear(kFirstAccount);
  Check(!clear_failed.ok(), "organization clear propagates storage removal failure");
}

void RejectsSemanticCollectionNamesOnLoad() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::OrganizationStore store(storage);
  auto snapshot = Example();
  snapshot.collections[0].name = "Alpha";
  snapshot.collections[1].name = "Bravo";
  Check(store.Save(kFirstAccount, snapshot).ok(), "semantic collection-name fixture saves");
  const auto stored = storage.Read(Path(kFirstAccount));
  Check(stored.ok(), "semantic collection-name fixture reads");
  if (!stored.ok()) return;

  auto whitespace = stored.value();
  Check(ReplaceBytes(whitespace, "Alpha", "     ") &&
            storage.WriteAtomicPrivate(Path(kFirstAccount), whitespace).ok(),
        "valid-checksum whitespace-name fixture writes");
  auto loaded = store.Load(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == z3cloudplay::StorageError::kInvalidData,
        "load rejects whitespace-only collection name");

  auto duplicate = stored.value();
  Check(ReplaceBytes(duplicate, "Bravo", "Alpha") &&
            storage.WriteAtomicPrivate(Path(kFirstAccount), duplicate).ok(),
        "valid-checksum duplicate-name fixture writes");
  loaded = store.Load(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == z3cloudplay::StorageError::kInvalidData,
        "load rejects duplicate collection names");
}

void PreservesOrphanedPersonalState() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::OrganizationStore store(storage);
  auto snapshot = Example();
  snapshot.selected_game_id = 999999;
  snapshot.notes[888888] = "Keep this when ownership metadata is omitted";
  snapshot.favorites.insert(777777);
  Check(store.Save(kFirstAccount, snapshot).ok(), "orphaned personal state saves");
  const auto loaded = store.Load(kFirstAccount);
  Check(loaded.ok() && loaded.value() && loaded.value()->selected_game_id == 999999 &&
            loaded.value()->notes.at(888888) == "Keep this when ownership metadata is omitted" &&
            loaded.value()->favorites.count(777777),
        "organization store does not require current ownership or drop omitted game IDs");
}

void ClearRemovesFinalAndTemporaryFiles() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::OrganizationStore store(storage);
  Check(store.Save(kFirstAccount, Example()).ok(), "clear fixture saves");
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount) + ".tmp", {'s', 't', 'a', 'l', 'e'}).ok(),
        "stale temporary fixture writes");
  Check(store.Clear(kFirstAccount).ok(), "organization clear succeeds");
  const auto missing = store.Load(kFirstAccount);
  Check(missing.ok() && !missing.value(), "cleared organization becomes missing");
  const auto temporary = storage.Read(Path(kFirstAccount) + ".tmp");
  Check(!temporary.ok() && temporary.error() == z3cloudplay::StorageError::kNotFound,
        "clear removes stale temporary file");
  Check(store.Clear(kFirstAccount).ok(), "organization clear is idempotent");
}

void SelectionDoesNotConsumeAnnotationCapacity() {
  TemporaryDirectory directory;z3cloudplay::Storage storage(directory.path());z3cloudplay::OrganizationStore store(storage);
  z3cloudplay::OrganizationSnapshot value;
  for(z3cloudplay::GameId id=1;id<=10000;++id)value.favorites.insert(id);
  value.selected_game_id=20000;
  Check(z3cloudplay::OrganizationSnapshotIsValid(value),"fixed selection does not consume the annotation ID budget");
  Check(store.Save(kFirstAccount,value).ok(),"full annotations preserve independent selection");
  const auto loaded=store.Load(kFirstAccount);
  Check(loaded.ok() && loaded.value() && loaded.value()->selected_game_id==20000,"selection round trips with full annotation capacity");
}

void SharedValidationMatchesEncodedBoundary() {
  TemporaryDirectory directory;z3cloudplay::Storage storage(directory.path());z3cloudplay::OrganizationStore store(storage);
  z3cloudplay::OrganizationSnapshot value;
  for(z3cloudplay::GameId id=1;id<=255;++id)value.notes[id]=std::string(4096,'x');
  value.notes[256]=std::string(1979,'x');
  Check(z3cloudplay::OrganizationSnapshotIsValid(value),"exact one MiB snapshot validates");
  Check(store.Save(kFirstAccount,value).ok(),"exact one MiB snapshot saves");
  const auto bytes=storage.Read(Path(kFirstAccount));
  Check(bytes.ok() && bytes.value().size()==1024*1024,"shared validator includes every encoded prefix and checksum");
  value.notes[256].push_back('x');
  Check(!z3cloudplay::OrganizationSnapshotIsValid(value),"one extra encoded byte is rejected before presentation apply");
  Check(!store.Save(kFirstAccount,value).ok(),"oversize snapshot cannot replace previous file");
  const auto restored=store.Load(kFirstAccount);
  Check(restored.ok() && restored.value() && restored.value()->notes.at(256).size()==1979,"failed replacement preserves prior snapshot");
}

}  // namespace

int main() {
  SelectionDoesNotConsumeAnnotationCapacity();
  SharedValidationMatchesEncodedBoundary();
  RoundTripAndIsolation();
  RejectsInvalidValuesWithoutReplacingGoodData();
  RejectsDamagedAndOversizedFiles();
  RejectsSemanticCollectionNamesOnLoad();
  PreservesOrphanedPersonalState();
  ClearRemovesFinalAndTemporaryFiles();
  if (failures) {
    std::fprintf(stderr, "%d organization assertion(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::puts("library_organization_test: all assertions passed");
  return EXIT_SUCCESS;
}
