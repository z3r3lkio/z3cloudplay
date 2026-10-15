// Compile-time interception is limited to the production settings translation
// unit. This file calls the real syscalls while injecting specific I/O failures.
#undef fsync
#undef openat
#undef rename
#undef renameat
#undef unlink
#undef unlinkat
#include "runtime/game_settings.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
namespace {
bool fail_file_sync = false, fail_directory_sync = false, fail_remove_config = false;
bool reject_relative_operations = false;
int config_renames_before_failure = -1;
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path);
  CHECK(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}
mode_t Mode(const std::filesystem::path& path) {
  struct stat status {};
  CHECK(::lstat(path.c_str(), &status) == 0);
  return status.st_mode & 0777;
}
extern "C" int z3cloudplay_test_openat(int fd, const char* name, int flags, ...) {
  if (reject_relative_operations) {
    errno = EINVAL;
    return -1;
  }
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list arguments;
    va_start(arguments, flags);
    mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
  }
  return ::openat(fd, name, flags, mode);
}
}
extern "C" int z3cloudplay_test_fsync(int fd) {
  struct stat status {};
  if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && fail_file_sync) {
    fail_file_sync = false;
    errno = EIO;
    return -1;
  }
  if (S_ISDIR(status.st_mode) && fail_directory_sync) {
    fail_directory_sync = false;
    errno = EIO;
    return -1;
  }
  return ::fsync(fd);
}
extern "C" int z3cloudplay_test_renameat(int from_dir, const char* from, int to_dir, const char* to) {
  if (std::string(to) == "dxvk.conf" && config_renames_before_failure >= 0 &&
      config_renames_before_failure-- == 0) {
    errno = EIO;
    return -1;
  }
  return ::renameat(from_dir, from, to_dir, to);
}
extern "C" int z3cloudplay_test_rename(const char* from, const char* to) {
  const auto name = std::filesystem::path(to).filename().string();
  if (name == "dxvk.conf" && config_renames_before_failure >= 0 &&
      config_renames_before_failure-- == 0) {
    errno = EIO;
    return -1;
  }
  return ::rename(from, to);
}
extern "C" int z3cloudplay_test_unlinkat(int fd, const char* name, int flags) {
  if (std::string(name) == "dxvk.conf" && fail_remove_config) {
    fail_remove_config = false;
    errno = EIO;
    return -1;
  }
  return ::unlinkat(fd, name, flags);
}
extern "C" int z3cloudplay_test_unlink(const char* path) {
  if (std::filesystem::path(path).filename() == "dxvk.conf" && fail_remove_config) {
    fail_remove_config = false;
    errno = EIO;
    return -1;
  }
  return ::unlink(path);
}
int main(int argc, char** argv) {
  using namespace z3cloudplay::runtime;
  namespace fs = std::filesystem;
  const fs::path source = argc > 1 ? argv[1] : ".";
  const auto root = fs::canonical(fs::temp_directory_path()) / ("z3cloudplay-settings-io-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "game"); fs::create_directories(root / "prefix");
  fs::permissions(root, fs::perms::owner_all);
  fs::permissions(root / "prefix", fs::perms::owner_all);
  const auto records = root / "settings.json";
  std::string reason;
  reject_relative_operations = true;
  CHECK(SaveGameSettingsFile(records.string(), {{220, {"", {}, 1}}}, &reason));
  CHECK(Mode(records) == 0600);
  const auto original = Read(records);
  fail_file_sync = true;
  CHECK(!SaveGameSettingsFile(records.string(), {{220, {"", {}, 2}}}, &reason));
  CHECK(Read(records) == original);  // no rename before the data durability barrier
  fail_directory_sync = true;
  CHECK(!SaveGameSettingsFile(records.string(), {{220, {"", {}, 2}}}, &reason));
  CHECK(!fail_directory_sync);  // a directory barrier was attempted after rename
  CHECK(SaveGameSettingsFile(records.string(), {{220, {"", {}, 2}}}, &reason));
  CHECK(::chmod(records.c_str(), 0644) == 0);  // admitted legacy owner file
  CHECK(SaveGameSettingsFile(records.string(), {{220, {"", {}, 3}}}, &reason));
  CHECK(Mode(records) == 0600);  // every replacement migrates to private mode
  std::map<std::uint32_t, GameSettingsRecord> loaded;
  CHECK(::chmod(records.c_str(), 0660) == 0);
  CHECK(!LoadGameSettingsFile(records.string(), &loaded, &reason));
  CHECK(::chmod(records.c_str(), 0600) == 0);

  const auto public_private_root = root / "public-settings";
  fs::create_directory(public_private_root);
  CHECK(!SaveGameSettingsFile((public_private_root / "settings.json").string(),
                              {{220, {"", {}, 4}}}, &reason));
  CHECK(!SaveGameSettingsFile("relative-settings.json", {{220, {"", {}, 4}}}, &reason));
  const auto linked_settings = root / "linked-settings.json";
  CHECK(::symlink(records.c_str(), linked_settings.c_str()) == 0);
  CHECK(!SaveGameSettingsFile(linked_settings.string(), {{220, {"", {}, 4}}}, &reason));

  const auto legacy_root = root / "legacy-public";
  const auto migrated_root = root / "migrated-private";
  fs::create_directory(legacy_root);
  fs::create_directory(migrated_root);
  fs::permissions(migrated_root, fs::perms::owner_all);
  const auto legacy = legacy_root / "game-settings.json";
  const auto migrated = migrated_root / "game-settings.json";
  CHECK(MigrateLegacyGameSettingsFile(
      (legacy_root / "missing.json").string(), migrated.string(), &reason));
  CHECK(!fs::exists(migrated));
  { std::ofstream(legacy) << original; }
  CHECK(::chmod(legacy.c_str(), 0644) == 0);
  CHECK(MigrateLegacyGameSettingsFile(legacy.string(), migrated.string(), &reason));
  CHECK(Read(legacy) == original);
  CHECK(Mode(legacy) == 0644);
  CHECK(Mode(migrated) == 0600);
  CHECK(!MigrateLegacyGameSettingsFile(legacy.string(), migrated.string(), &reason));
  CHECK(reason.find("already exist") != std::string::npos);
  const auto malformed = legacy_root / "malformed.json";
  { std::ofstream(malformed) << "{"; }
  CHECK(!MigrateLegacyGameSettingsFile(
      malformed.string(), (migrated_root / "other.json").string(), &reason));
  CHECK(!fs::exists(migrated_root / "other.json"));
  const auto unsafe_mode = legacy_root / "unsafe-mode.json";
  { std::ofstream(unsafe_mode) << original; }
  CHECK(::chmod(unsafe_mode.c_str(), 0660) == 0);
  CHECK(!MigrateLegacyGameSettingsFile(
      unsafe_mode.string(), (migrated_root / "unsafe.json").string(), &reason));

  GameRecipe recipe;
  CHECK(ParseGameRecipe(Read(source / "runtime/recipes/steam-220-hl2-v1.json"), &recipe, &reason));
  SettingsLaunchInput input;
  input.recipe = &recipe;
  input.target = {"z3cloudplay-steam-220", (root / "game").string()};
  input.kit = KitCapabilitiesFromManifest({kKitDxvkConfig}, true);
  input.record.overrides = {{"frame_limit", "30"}, {"windows_version", "win7"}};
  input.runtime_dir = (source / "runtime").string();
  SettingsLaunchPlan plan;
  CHECK(BuildSettingsLaunchPlan(input, &plan, &reason));
  std::ofstream(root / "prefix/user.reg") << "malformed registry";
  const std::string old_config = "# Generated by Z3CloudPlay from this game's settings. old\n";
  std::ofstream(root / "game/dxvk.conf") << old_config;
  config_renames_before_failure = 1; // publish new config, then refuse rollback's rename
  CHECK(!ApplySettingsFiles(plan, (root / "game").string(), (root / "prefix").string(), 1, &reason));
  CHECK(reason.find("rollback failed") != std::string::npos);
  CHECK(reason.find("reconciliation required") != std::string::npos);
  CHECK(Read(root / "game/dxvk.conf") == plan.dxvk_config);

  fs::remove(root / "game/dxvk.conf");
  config_renames_before_failure = -1;
  fail_remove_config = true;
  CHECK(!ApplySettingsFiles(plan, (root / "game").string(), (root / "prefix").string(), 1, &reason));
  CHECK(reason.find("rollback failed") != std::string::npos);
  CHECK(reason.find("reconciliation required") != std::string::npos);
  CHECK(Read(root / "game/dxvk.conf") == plan.dxvk_config);
  CHECK(Read(root / "prefix/user.reg") == "malformed registry");
  fs::remove_all(root);
  std::puts("settings I/O: durability and failed restore/remove diagnostics pass");
}
