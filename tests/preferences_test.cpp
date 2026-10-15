#include "app/preferences.h"
#include "platform/storage.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/z3cloudplay-preferences-XXXXXX";
    const char* created = mkdtemp(pattern);
    Check(created != nullptr, "creates a temporary test directory");
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    if (path_.empty()) {
      return;
    }
    std::remove((path_ + "/preferences.conf.tmp").c_str());
    std::remove((path_ + "/preferences.conf").c_str());
    rmdir(path_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

void StorageRejectsPathsOutsideItsRoot() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  const z3cloudplay::Bytes data{'x'};

  const auto parent = storage.WriteAtomic("../outside", data);
  Check(!parent.ok(), "rejects parent traversal");
  Check(parent.error() == z3cloudplay::StorageError::kInvalidPath,
        "reports traversal as an invalid path");

  const auto absolute = storage.Read("/etc/passwd");
  Check(!absolute.ok(), "rejects absolute paths");
  Check(absolute.error() == z3cloudplay::StorageError::kInvalidPath,
        "reports absolute paths as invalid");

  const auto windows = storage.Read("folder\\..\\secret");
  Check(!windows.ok(), "rejects platform-ambiguous separators");
}

void StorageDistinguishesMissingFiles() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());

  const auto result = storage.Read("missing.bin");

  Check(!result.ok(), "missing read fails");
  Check(result.error() == z3cloudplay::StorageError::kNotFound,
        "missing read has a distinct error");
  Check(result.message().find("missing.bin") != std::string::npos,
        "missing error identifies the file");
}

void StorageWritesAndReadsBytesAtomically() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  const z3cloudplay::Bytes expected{0x00, 0x41, 0xff, 0x0a};

  const auto written = storage.WriteAtomic("preferences.conf", expected);
  Check(written.ok(), "atomic write succeeds");

  const auto read = storage.Read("preferences.conf");
  Check(read.ok(), "written file can be read");
  if (read.ok()) {
    Check(read.value() == expected, "read preserves every byte");
  }

  struct stat ignored {};
  Check(stat((directory.path() + "/preferences.conf.tmp").c_str(), &ignored) != 0,
        "atomic write leaves no temporary file");
}

void StorageRejectsOversizedReads() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  const z3cloudplay::Bytes oversized(1024 * 1024 + 1, 0x7f);
  Check(storage.WriteAtomic("preferences.conf", oversized).ok(),
        "oversized read fixture is written");

  const auto read = storage.Read("preferences.conf");

  Check(!read.ok(), "read rejects files above its memory bound");
  Check(read.error() == z3cloudplay::StorageError::kReadFailed,
        "oversized file is reported as a read failure");
  Check(read.message().find("1048576") != std::string::npos,
        "oversized read error reports the byte limit");
}

void MissingPreferencesUseDefaultsWithoutAnError() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::PreferencesStore store(storage);

  const auto loaded = store.Load();

  Check(!loaded.preferences.reduce_motion, "reduce motion defaults to false");
  Check(loaded.status == z3cloudplay::PreferencesLoadStatus::kDefaultMissing,
        "missing preferences are classified separately");
  Check(loaded.diagnostic.empty(), "first run does not show an error");
}

void PreferencesPersistAcrossStoreInstances() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::PreferencesStore first_store(storage);
  z3cloudplay::Preferences preferences;
  Check(first_store.Save(preferences).ok(), "initial preference save succeeds");
  preferences.reduce_motion = true;

  const auto saved = first_store.Save(preferences);
  Check(saved.ok(), "preference replacement succeeds");

  z3cloudplay::PreferencesStore restarted_store(storage);
  const auto loaded = restarted_store.Load();
  Check(loaded.preferences.reduce_motion, "reduce motion survives restart");
  Check(loaded.status == z3cloudplay::PreferencesLoadStatus::kLoaded,
        "valid preferences report a loaded status");
  Check(loaded.diagnostic.empty(), "valid preferences have no diagnostic");
}

void TitleStoreSuffixPersistsAndVersionOneStillLoads() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  z3cloudplay::PreferencesStore store(storage);
  z3cloudplay::Preferences preferences;
  preferences.title_store_suffix = true;
  Check(store.Save(preferences).ok(), "suffix preference saves");
  const auto loaded = store.Load();
  Check(loaded.status == z3cloudplay::PreferencesLoadStatus::kLoaded && loaded.preferences.title_store_suffix &&
            !loaded.preferences.reduce_motion,
        "title store suffix survives restart");
  const std::string old_format = "version=1\nreduce_motion=1\n";
  Check(storage.WriteAtomic("preferences.conf", z3cloudplay::Bytes(old_format.begin(), old_format.end())).ok(),
        "version 1 fixture is written");
  const auto legacy = store.Load();
  Check(legacy.status == z3cloudplay::PreferencesLoadStatus::kLoaded && legacy.preferences.reduce_motion &&
            !legacy.preferences.title_store_suffix,
        "version 1 preferences still load, without the suffix");
  const std::string version_two = "version=2\nreduce_motion=0\ntitle_store_suffix=1\n";
  Check(storage.WriteAtomic("preferences.conf", z3cloudplay::Bytes(version_two.begin(), version_two.end())).ok(),
        "version 2 fixture is written");
  const auto two = store.Load();
  Check(two.status == z3cloudplay::PreferencesLoadStatus::kLoaded && two.preferences.title_store_suffix &&
            !two.preferences.cloud_saves_auto,
        "version 2 preferences load with automatic Cloud saves off (it is opt-in)");
  z3cloudplay::Preferences manual;
  manual.cloud_saves_auto = false;
  Check(store.Save(manual).ok(), "manual Cloud saves preference saves");
  const auto off = store.Load();
  Check(off.status == z3cloudplay::PreferencesLoadStatus::kLoaded && !off.preferences.cloud_saves_auto,
        "automatic Cloud saves off survives restart");
}

void CorruptPreferencesRestoreDefaultsWithVisibleDiagnostic() {
  TemporaryDirectory directory;
  z3cloudplay::Storage storage(directory.path());
  const z3cloudplay::Bytes corrupt{'n', 'o', 't', ' ', 'v', 'a', 'l', 'i', 'd'};
  Check(storage.WriteAtomic("preferences.conf", corrupt).ok(),
        "corrupt fixture is written");
  z3cloudplay::PreferencesStore store(storage);

  const auto loaded = store.Load();

  Check(!loaded.preferences.reduce_motion,
        "corrupt preferences restore reduce motion default");
  Check(loaded.status == z3cloudplay::PreferencesLoadStatus::kDefaultCorrupt,
        "corruption is distinct from a missing preference file");
  Check(!loaded.diagnostic.empty(), "corruption produces a visible diagnostic");
  Check(loaded.diagnostic.find("corrupt") != std::string::npos,
        "diagnostic explains that preferences are corrupt");
}

void PreferencesPrepareIndependentRoot() {
  TemporaryDirectory parent;
  const auto root=parent.path()+"/preferences-root";
  z3cloudplay::Storage storage(root);
  z3cloudplay::PreferencesStore preferences(storage);
  Check(preferences.Save({true}).ok(),"preferences prepare a root independently of authentication");
  Check(preferences.Load().preferences.reduce_motion,"prepared preferences restore");
  struct stat info{};
  Check(stat(root.c_str(),&info)==0 && (info.st_mode & 0777)==0700,"new preference root is private");
  std::remove((root+"/preferences.conf").c_str());rmdir(root.c_str());
}

}  // namespace

int main() {
  PreferencesPrepareIndependentRoot();
  StorageRejectsPathsOutsideItsRoot();
  StorageDistinguishesMissingFiles();
  StorageWritesAndReadsBytesAtomically();
  StorageRejectsOversizedReads();
  MissingPreferencesUseDefaultsWithoutAnError();
  PreferencesPersistAcrossStoreInstances();
  CorruptPreferencesRestoreDefaultsWithVisibleDiagnostic();
  TitleStoreSuffixPersistsAndVersionOneStillLoads();

  if (failures != 0) {
    std::fprintf(stderr, "%d preference test assertion(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::puts("preferences_test: all assertions passed");
  return EXIT_SUCCESS;
}
