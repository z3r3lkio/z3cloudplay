#include <algorithm>
#include "runtime/package_installer.h"
#include "runtime/private_directories.h"
#include "runtime/publication_contract.h"
#include "pw_publication_sha256.h"
#include <array>
#include <cstring>
#include <sys/file.h>
#include <fcntl.h>
#ifdef Z3CLOUDPLAY_TEST_SETTINGS_OWNER_INTEGRATION
#include "runtime/settings_publication_owner.h"
#include <memory>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <csignal>
#include <unistd.h>
#include <vector>

#ifdef Z3CLOUDPLAY_TEST_AT_UNAVAILABLE
extern "C" int z3cloudplay_ps5_file_lock(int fd, int operation) { return ::flock(fd, operation); }
#include <cerrno>
extern "C" int openat(int, const char*, int, ...) { errno = EINVAL; return -1; }
extern "C" int mkdirat(int, const char*, mode_t) { errno = EINVAL; return -1; }
extern "C" int fstatat(int, const char*, struct stat*, int) { errno = EINVAL; return -1; }
extern "C" int unlinkat(int, const char*, int) { errno = EINVAL; return -1; }
#endif

using namespace z3cloudplay::runtime;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "z3cloudplay-package-installer-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char* made = ::mkdtemp(writable.data());
    CHECK(made != nullptr);
    // macOS temp_directory_path may use the /var symlink alias. The managed
    // contract rejects every supplied symlink component; use the real fixture.
    path = std::filesystem::canonical(made);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

void Write(const std::filesystem::path& path, const std::string& bytes,
           unsigned mode = 0644) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << bytes;
  stream.close();
  CHECK(::chmod(path.c_str(), mode) == 0);
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string RealHashBytes(const std::string& bytes) {
  std::array<uint8_t, 32> digest{}; CHECK(pw_publication_sha256_bytes(bytes.data(), bytes.size(), digest.data()) == 0);
  constexpr char hex[] = "0123456789abcdef"; std::string result(64, '0');
  for (std::size_t i = 0; i < digest.size(); ++i) { result[2 * i] = hex[digest[i] >> 4]; result[2 * i + 1] = hex[digest[i] & 15]; }
  return result;
}
// Both immutable kit fixtures and completion metadata use the pinned real SHA.
std::optional<std::string> TestHash(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary); if (!file) return std::nullopt;
  const std::string bytes{std::istreambuf_iterator<char>(file), {}}; return RealHashBytes(bytes);
}
std::optional<std::string> TestHashBytes(std::string_view bytes) { return RealHashBytes(std::string(bytes)); }

template<std::size_t N> void Copy(char (&out)[N], const std::string& value) {
  CHECK(value.size() < N); std::memcpy(out, value.c_str(), value.size() + 1);
}
// Test-only complete publication fixture, not a production transaction driver.
// Real pending/durable/OWNER interruption coverage lives in the coordinator suite.
PackageActionResult WriteCompleteFixture(const std::filesystem::path& host_data, const std::string& kit_id,
    const RuntimeInstall& record, const PublicationPaths& output, const ProfilePublication& value, bool public_profile = false) {
  const auto game = host_data / std::filesystem::path(record.data_root).lexically_relative("/data");
  const auto control = game / ".z3cloudplay-control";
  if (std::filesystem::exists(output.descriptor) && !std::filesystem::exists(output.metadata))
    return {PackageActionState::kFailed, "publication-collision", "foreign fixture descriptor"};
  for (const auto& directory : {output.profile.parent_path(), output.input_directory, control}) {
    if (std::filesystem::exists(directory) && !IsPrivateDirectory(directory))
      return {PackageActionState::kFailed, "private-directory", "foreign fixture publication directory"};
    std::filesystem::create_directories(directory); CHECK(chmod(directory.c_str(), 0700) == 0);
  }
  if (!std::filesystem::exists(control / "publication.lock")) Write(control / "publication.lock", "", 0600);
  Write(game / "library/prefixes/game/user.reg", "WINE REGISTRY Version 2\n", 0600);
  // The game root is the recorded download path, mapped into the same host namespace as /data.
  std::filesystem::create_directories(host_data.parent_path() / std::filesystem::path(record.install_path).relative_path());
  Write(output.profile, value.profile_bytes, public_profile ? 0644 : 0600);
  const auto catalog = value.profile_id + ".profile\n"; Write(output.catalog, catalog, 0600);
  for (const auto& preset : value.input_presets) Write(output.input_directory / preset.filename, preset.bytes, 0600);
  Write(output.descriptor, value.descriptor_bytes, 0600);
  PwTitlePublicationMetadata metadata{}; metadata.schema_version = 2;
  Copy(metadata.install_id, record.install_id); Copy(metadata.title_id, record.title_id); Copy(metadata.data_root, record.data_root);
  Copy(metadata.profile_id, value.profile_id); Copy(metadata.runtime_bundle_id, record.runtime_bundle_id);
  Copy(metadata.recipe_id, record.recipe_id); Copy(metadata.recipe_version, record.recipe_version); Copy(metadata.kit_id, kit_id);
  metadata.settings_revision = record.settings_revision;
  Copy(metadata.profile_sha256, RealHashBytes(value.profile_bytes)); Copy(metadata.descriptor_sha256, RealHashBytes(value.descriptor_bytes));
  Copy(metadata.catalog_sha256, RealHashBytes(catalog)); metadata.input_count = value.input_presets.size();
  for (std::size_t i = 0; i < value.input_presets.size(); ++i) {
    Copy(metadata.inputs[i].name, value.input_presets[i].filename); Copy(metadata.inputs[i].sha256, RealHashBytes(value.input_presets[i].bytes));
  }
  Copy(metadata.dxvk_path, record.install_path + "/dxvk.conf"); metadata.registry_count = 2;
  Copy(metadata.registry[0].key, "Software\\Wine\\AppDefaults\\hl2.exe"); Copy(metadata.registry[0].name, "Version");
  Copy(metadata.registry[1].key, "Control Panel\\Desktop"); Copy(metadata.registry[1].name, "LogPixels");
  metadata.registry[0].kind = metadata.registry[1].kind = PW_TITLE_PUBLICATION_REGISTRY_ABSENT;
  auto bytes = SerializePublicationMetadata(metadata); CHECK(bytes); Write(output.metadata, *bytes, 0600);
  PwTitlePublication receipt{}; receipt.schema_version = 1; receipt.phase = PW_TITLE_PUBLICATION_COMMITTED;
  Copy(receipt.transaction_id, "00000000-0000-7000-8000-000000000001"); Copy(receipt.install_id, record.install_id);
  Copy(receipt.title_id, record.title_id); Copy(receipt.kit_id, kit_id); Copy(receipt.runtime_bundle_id, record.runtime_bundle_id);
  Copy(receipt.recipe_id, record.recipe_id); Copy(receipt.recipe_version, record.recipe_version);
  receipt.settings_revision = record.settings_revision; Copy(receipt.plan_sha256, std::string(64, 'c'));
  Copy(receipt.publication_sha256, RealHashBytes(*bytes)); auto state = SerializePublicationReceipt(receipt); CHECK(state);
  Write(control / "publication-state.v1", *state, 0600);
  return {PackageActionState::kComplete, {}, {}};
}

RuntimeInstall Record() {
  RuntimeInstall record;
  record.install_id = "install-hl2";
  record.title_id = "FAKE78001";
  record.app_id = 220;
  record.data_root = "/data/z3cloudplay/games/install-hl2";
  record.default_profile_id = "game";
  record.runtime_bundle_id = "wine-wow64";
  record.recipe_id = "steam-220-hl2-v1";
  record.recipe_version = "1";
  record.settings_revision = std::numeric_limits<std::uint64_t>::max();
  record.exit_policy = "exit-title";
  record.launch_entry_id = "0";
  record.launch_path = "hl2.exe";
  record.install_path = "/mnt/games/220";
  record.volume_id = "internal";
  return record;
}

struct Fixture {
  TemporaryDirectory temporary;
  std::filesystem::path kit = temporary.path / "kit";
  std::filesystem::path host_data = temporary.path / "data";
  VerifiedKitView view;
  PackageInstallPaths paths;
  bool canceled = false;
  int cancel_after_checks = -1;
  int cancel_checks = 0;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;  // (done, total) reports
  std::uint64_t free_bytes = 1ULL << 30;
  int writes = 0;
  int sync_calls = 0;
  int fail_sync_call = -1;
  bool fail_title_post_rename_once = false;
  bool fail_bottle_post_rename_once = false;
  bool publish_public_profile = false;
  bool fail_publication = false;
  bool legacy_descriptor = false;

  Fixture() {
    Write(kit / "titles/variant/eboot.bin", "native", 0755);
    Write(kit / "titles/variant/sce_sys/param.json", "title", 0644);
    Write(kit / "runtime/wine/rev/lib/core.prx", "runtime", 0755);
    Write(kit / "template/bottle/drive_c/windows/system.reg", "registry", 0644);
    Write(kit / "template/bottle/dosdevices/.pw-symlinks",
          "c:\t../drive_c\nz:\t/\n", 0644);
    Write(kit / "recipes/hl2.json", "recipe", 0644);

    view.root = kit;
    view.kit_id = "kit-1";
    view.files_sha256 = std::string(64, 'a');
    view.runtime = {"wine-wow64", "rev", "runtime/wine/rev"};
    view.template_path = "template/bottle";
    view.template_drive_table = "template/bottle/dosdevices/.pw-symlinks";
    view.recipe = {"steam-220-hl2-v1", "1", "recipes/hl2.json",
                   *TestHash(kit / "recipes/hl2.json")};
    view.variants.push_back({"variant", "FAKE78001", "titles/variant"});
    for (const auto& entry : std::vector<std::pair<std::string, unsigned>>{
             {"titles/variant/eboot.bin", 0755},
             {"titles/variant/sce_sys/param.json", 0644},
             {"runtime/wine/rev/lib/core.prx", 0755},
             {"template/bottle/drive_c/windows/system.reg", 0644},
             {"template/bottle/dosdevices/.pw-symlinks", 0644},
             {"recipes/hl2.json", 0644}}) {
      const auto path = kit / entry.first;
      view.files.push_back(
          {entry.first, std::filesystem::file_size(path), *TestHash(path), entry.second});
    }
    view.artifacts = {
        {"runtime", "rev", *TestHash(kit / "runtime/wine/rev/lib/core.prx"),
         ImmutableArtifactLocation::kStagedTitle, "win/wine/lib/core.prx"},
        {"recipe", "steam-220-hl2-v1-1", *TestHash(kit / "recipes/hl2.json"),
         ImmutableArtifactLocation::kKit, "recipes/hl2.json"}};
    paths.console_data_root = "/data";
    paths.host_data_root = host_data;
  }

  ProfilePublication Publication(const RuntimeInstall& record) const {
    ProfilePublication publication;
    publication.install_id = record.install_id;
    publication.title_id = record.title_id;
    publication.data_root = record.data_root;
    publication.profile_id = record.default_profile_id;
    publication.runtime_bundle_id = record.runtime_bundle_id;
    publication.recipe_id = record.recipe_id;
    publication.recipe_version = record.recipe_version;
    publication.settings_revision = record.settings_revision;
    std::string windows_root = "Z:" + record.install_path;
    std::replace(windows_root.begin(), windows_root.end(), '/', '\\');
    publication.profile_bytes = "[application]\nid = game\nname = Half-Life 2\nprefix = game\n"
        "executable = " + windows_root + "\\hl2.exe\n"
        "working_directory = " + windows_root + "\n"
        "runtime = wine-wow64\narchitecture = pe64\ngraphics = dxvk\n";
    publication.profile_references_validated = true;
    publication.descriptor_bytes =
        "{\"schema_version\":2,\"install_id\":\"install-hl2\","
        "\"title_id\":\"FAKE78001\",\"app_id\":220,"
        "\"data_root\":\"/data/z3cloudplay/games/install-hl2\","
        "\"default_profile_id\":\"game\","
        "\"settings_revision\":18446744073709551615,"
        "\"runtime_bundle_id\":\"wine-wow64\","
        "\"recipe_id\":\"steam-220-hl2-v1\",\"recipe_version\":\"1\","
        "\"exit_policy\":\"exit-title\",\"game_root\":\"" + record.install_path + "\"}";
    return publication;
  }

  FilesystemPackageInstaller Installer(bool inject_sync = true) {
    PackageInstallerOperations operations;
    operations.hash_file = TestHash;
    operations.hash_bytes = TestHashBytes;
    operations.free_bytes = [&](const std::filesystem::path&) {
      return std::optional<std::uint64_t>(free_bytes);
    };
    operations.canceled = [&] {
      ++cancel_checks;
      return canceled ||
             (cancel_after_checks >= 0 && cancel_checks > cancel_after_checks);
    };
    if (inject_sync) {
      operations.sync_path = [&](const std::filesystem::path& path, bool) {
        ++sync_calls;
        if (fail_title_post_rename_once &&
            std::filesystem::exists(host_data / "homebrew/FAKE78001") &&
            path == host_data / "homebrew") {
          fail_title_post_rename_once = false;
          return false;
        }
        if (fail_bottle_post_rename_once &&
            std::filesystem::exists(host_data /
                                    "z3cloudplay/games/install-hl2/library/prefixes/game") &&
            path == host_data / "z3cloudplay/games/install-hl2/library/prefixes") {
          fail_bottle_post_rename_once = false;
          return false;
        }
        return fail_sync_call < 0 || sync_calls != fail_sync_call;
      };
    }
    operations.progress = [&](std::uint64_t done, std::uint64_t total) { progress.emplace_back(done, total); };
    operations.validate_bottle = [](const std::filesystem::path& bottle) {
      return std::filesystem::is_regular_file(bottle / "drive_c/windows/system.reg");
    };
    operations.provide_publication = [&](const RuntimeInstall& record) {
      auto publication = Publication(record);
      if (legacy_descriptor) {  // Schema 1: no explicit root, historical per-title root inferred.
        auto& bytes = publication.descriptor_bytes;
        bytes.replace(bytes.find("\"schema_version\":2"), 18, "\"schema_version\":1");
        bytes.erase(bytes.find(",\"game_root\""), std::string::npos); bytes += "}";
      }
      return std::optional<ProfilePublication>(publication);
    };
    operations.publish_owned = [&](const RuntimeInstall& record, const PublicationPaths& output,
                                   const ProfilePublication& publication) {
      ++writes;
      if (fail_publication) return PackageActionResult{PackageActionState::kFailed, "publisher-fault", "injected driver failure"};
      return WriteCompleteFixture(host_data, view.kit_id, record, output, publication, publish_public_profile);
    };
    return FilesystemPackageInstaller(view, paths, std::move(operations));
  }
};

void LegacyDescriptorOnlyBindsHistoricalRoot() {
  for (const auto* root : {"/mnt/games/220", "/data/homebrew/FAKE78001/games/220"}) {
    Fixture fixture; auto record = Record(); record.install_path = root;
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    fixture.legacy_descriptor = true;
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    const bool historical = std::string(root) == "/data/homebrew/FAKE78001/games/220";
    CHECK((installer.PublishProfile(record).state == PackageActionState::kComplete) == historical);
    CHECK(fixture.writes == (historical ? 1 : 0));
  }
}

void TemplateCopyReportsProgress() {
  Fixture fixture; auto record = Record();
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  fixture.progress.clear();
  CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
  // From nothing to all of it, never backwards, against one total: hash, copy and verify.
  CHECK(fixture.progress.size() >= 3);
  const auto total = fixture.progress.front().second;
  CHECK(total > 0 && fixture.progress.front().first == 0);
  for (std::size_t index = 1; index < fixture.progress.size(); ++index)
    CHECK(fixture.progress[index].second == total && fixture.progress[index].first >= fixture.progress[index - 1].first);
  CHECK(fixture.progress.back().first == total);
}

void CommittedReceiptIsRequired() {
  Fixture fixture; auto record = Record();
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
  CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete);
  std::filesystem::remove(fixture.host_data / "z3cloudplay/games/install-hl2/.z3cloudplay-control/publication-state.v1");
  CHECK(!installer.Inspect(record).profile_published);
}

void FailedDriverCreatesNoPublicationDirectories() {
  Fixture fixture; auto record = Record();
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
  fixture.fail_publication = true;
  CHECK(installer.PublishProfile(record).code == "publisher-fault" && fixture.writes == 1);
  const auto library = fixture.host_data / "z3cloudplay/games/install-hl2/library";
  CHECK(!std::filesystem::exists(library / "profiles") && !std::filesystem::exists(library / "input"));
  CHECK(!std::filesystem::exists(fixture.host_data / "homebrew/FAKE78001/game-launch.json"));
  CHECK(!std::filesystem::exists(library.parent_path() / ".z3cloudplay-control"));
}
void CompleteInspectorRefusesMixedSettingsAndBusyPublisher() {
  for (unsigned damage = 0; damage < 9; ++damage) {
    Fixture fixture; auto record = Record();
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete && installer.Inspect(record).profile_published);
    const auto game = fixture.host_data / "z3cloudplay/games/install-hl2";
    const auto control = game / ".z3cloudplay-control"; const auto state_path = control / "publication-state.v1";
    const auto metadata_path = game / "library/profiles/.z3cloudplay-publication";
    const auto receipt_bytes = Read(state_path); PwTitlePublication receipt{};
    CHECK(pw_title_publication_parse(reinterpret_cast<const uint8_t*>(receipt_bytes.data()), receipt_bytes.size(), &receipt) == PW_OK);
    if (damage == 0) { receipt.phase = PW_TITLE_PUBLICATION_PENDING; receipt.publication_sha256[0] = 0; }
    if (damage == 1) Write(state_path, "corrupt", 0600);
    if (damage == 2) Write(game / "library/prefixes/game/user.reg", "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop] 0\n\"LogPixels\"=dword:00000078\n", 0600);
    if (damage == 3) Write(fixture.host_data.parent_path() / std::filesystem::path(record.install_path).relative_path() / "dxvk.conf", "foreign", 0644);
    if (damage == 4) { const auto metadata = Read(metadata_path); auto legacy = metadata; CHECK(legacy.find("schema=2") == 0); legacy[7] = '1'; Write(metadata_path, legacy, 0600); Copy(receipt.publication_sha256, RealHashBytes(legacy)); }
    if (damage == 5) {
      const auto bytes = Read(metadata_path); PwTitlePublicationMetadata metadata{};
      CHECK(pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &metadata) == PW_OK);
      Copy(metadata.kit_id, "foreign-kit"); Copy(receipt.kit_id, "foreign-kit");
      auto changed = SerializePublicationMetadata(metadata); CHECK(changed); Write(metadata_path, *changed, 0600); Copy(receipt.publication_sha256, RealHashBytes(*changed));
    }
    if (damage == 6) CHECK(chmod((control / "publication.lock").c_str(), 0644) == 0);
    if (damage == 7) CHECK(link(state_path.c_str(), (fixture.temporary.path / "foreign-link").c_str()) == 0);
    if (damage == 8) Write(game / "library/profiles/profiles.lst", "game.profile\nforeign.profile\n", 0600);
    if (damage == 0 || damage == 4 || damage == 5) { auto changed = SerializePublicationReceipt(receipt); CHECK(changed); Write(state_path, *changed, 0600); }
    CHECK(!installer.Inspect(record).profile_published);
  }
  Fixture fixture; auto record = Record();
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  auto installer = fixture.Installer(); CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
  CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete);
  const auto game = fixture.host_data / "z3cloudplay/games/install-hl2";
  // Unrelated Wine registry rewrites are accepted by semantic admission.
  Write(game / "library/prefixes/game/user.reg", "WINE REGISTRY Version 2\n\n[Software\\\\Unrelated] 42\n\"Keep\"=\"value\"\n", 0600);
  CHECK(installer.Inspect(record).profile_published);
  const int fd = open((game / ".z3cloudplay-control/publication.lock").c_str(), O_RDONLY); CHECK(fd >= 0);
  CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0); CHECK(!installer.Inspect(record).profile_published);
  CHECK(flock(fd, LOCK_SH | LOCK_NB) == 0); CHECK(installer.Inspect(record).profile_published);
  CHECK(close(fd) == 0 && installer.Inspect(record).profile_published);
}

#ifdef Z3CLOUDPLAY_TEST_SETTINGS_OWNER_INTEGRATION
void RealFilesystemInstallerAndOwnerRecoverPendingPublication() {
  Fixture fixture; auto record = Record(); record.install_path = "/data/homebrew/FAKE78001/games/220";
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  const auto owner_root = fixture.temporary.path / "owner"; std::filesystem::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
  z3cloudplay::Storage storage(owner_root.string());
  auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
  RuntimeInstallRepository repository(storage, "installs.json", availability); CHECK(repository.Load().ok());
  ReserveRequest request; request.install_id = record.install_id; request.app_id = record.app_id;
  request.default_profile_id = record.default_profile_id; request.runtime_bundle_id = record.runtime_bundle_id;
  request.recipe_id = record.recipe_id; request.recipe_version = record.recipe_version; request.settings_revision = record.settings_revision;
  request.launch_entry_id = record.launch_entry_id; request.launch_path = record.launch_path;
  request.install_path = record.install_path; request.volume_id = record.volume_id; request.kit_title_variants = {record.title_id};
  CHECK(repository.Reserve(request).ok() && repository.Update(record).ok()); RepositoryInstallRecordStore records(repository);
  struct Native final : NativeCoordinator {
    bool installed = false; unsigned submissions = 0;
    NativeActivity Activity(const std::string&) override { return NativeActivity::kStopped; }
    NativeStatus Query(const RuntimeInstall&) override { return {true, installed, true, true, false, false}; }
    RegistrationResult Register(const RuntimeInstall&) override { installed = true; ++submissions; return {RegistrationResultState::kAccepted, {}, {}}; }
  } native;
  std::unique_ptr<SettingsPublicationOwner> owner; unsigned publications = 0;
  PackageInstallerOperations operations; operations.hash_file = TestHash; operations.hash_bytes = TestHashBytes;
  operations.free_bytes = [](const std::filesystem::path&) { return std::optional<std::uint64_t>(1ULL << 30); };
  operations.validate_bottle = [](const std::filesystem::path& bottle) { return std::filesystem::is_regular_file(bottle / "drive_c/windows/system.reg"); };
  operations.provide_publication = [&](const RuntimeInstall& r) { return std::optional<ProfilePublication>(fixture.Publication(r)); };
  operations.publish_owned = [&](const RuntimeInstall& r, const PublicationPaths& paths, const ProfilePublication& publication) {
    ++publications;
    CHECK(!std::filesystem::exists(paths.profile.parent_path()) && !std::filesystem::exists(paths.input_directory));
    SettingsLaunchPlan plan; plan.app_id = r.app_id; plan.recipe_id = r.recipe_id; plan.recipe_version = r.recipe_version;
    plan.profile_id = r.default_profile_id; plan.profile_bytes = publication.profile_bytes; plan.settings_revision = r.settings_revision;
    plan.launch_entry_id = r.launch_entry_id; plan.launch_entry = {r.launch_entry_id, "Play", r.launch_path, r.launch_arguments};
    auto prepared = owner->Prepare(r, plan, fixture.view.kit_id, "00000000-0000-7000-8000-000000000001", 1760000000, fixture.temporary.path);
    if (!prepared.ok()) std::fprintf(stderr, "Filesystem Prepare: %s\n", prepared.message().c_str()); CHECK(prepared.ok());
    auto dependencies = owner->Dependencies(fixture.temporary.path);
    dependencies.after_step = [](PublicationStep step) { return step != PublicationStep::kPendingDurable; };
    return PublishSettingsOperation(prepared.value(), paths, dependencies);
  };
  FilesystemPackageInstaller installer(fixture.view, fixture.paths, std::move(operations));
  owner = std::make_unique<SettingsPublicationOwner>(records, installer, native);
  RuntimeInstallService service(records, installer, native, [](const RuntimeInstall&) { return CompatibilityResult{true, {}}; }, {},
      [&](const RuntimeInstall& r) { return r.journal->operation == "prepare" ? installer.PublishProfile(r) : owner->Resume(r, fixture.temporary.path); });
  CHECK(service.Prepare(record.install_id).ok()); CHECK(service.Step(record.install_id).ok());
  // Downloaded game data follows completed title staging and precedes settings.
  std::filesystem::create_directories(fixture.host_data / "homebrew/FAKE78001/games/220");
  CHECK(service.Step(record.install_id).ok()); CHECK(service.Step(record.install_id).ok());
  const auto bottle = fixture.host_data / "z3cloudplay/games/install-hl2/library/prefixes/game";
  Write(bottle / "user.reg", "WINE REGISTRY Version 2\n", 0600); Write(bottle / "drive_c/save.dat", "player-save", 0600);
  CHECK(!service.Step(record.install_id).ok() && publications == 1 && native.submissions == 0);
  auto blocked = records.Find(record.install_id); CHECK(blocked && blocked->journal && blocked->journal->operation == "settings-publication");
  CHECK(!installer.Inspect(*blocked).profile_published);
  // Cold repository/OWNER/service recovery uses the same real filesystem
  // installer and shared admission; it does not call PublishProfile again.
  RuntimeInstallRepository restart(storage, "installs.json", availability); CHECK(restart.Load().ok());
  RepositoryInstallRecordStore restarted_records(restart); SettingsPublicationOwner resumed(restarted_records, installer, native);
  RuntimeInstallService restarted(restarted_records, installer, native, {}, {},
      [&](const RuntimeInstall& r) { return resumed.Resume(r, fixture.temporary.path); });
  CHECK(restarted.Step(record.install_id).ok() && publications == 1 && native.submissions == 1);
  auto ready = restarted_records.Find(record.install_id);
  CHECK(ready && ready->state == PreparationState::kReady && !ready->journal && installer.Inspect(*ready).profile_published);
  CHECK(Read(bottle / "drive_c/save.dat") == "player-save" && Read(bottle / "user.reg") == "WINE REGISTRY Version 2\n");
}
#endif

void CompletePublicationAndFreshInspection() {
  Fixture fixture;
  auto record = Record();
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  const auto title = fixture.host_data / "homebrew/FAKE78001";
  CHECK(Read(title / "eboot.bin") == "native");
  CHECK(Read(title / "win/wine/lib/core.prx") == "runtime");
  CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
  const auto bottle = fixture.host_data /
                      "z3cloudplay/games/install-hl2/library/prefixes/game";
  CHECK(Read(bottle / "dosdevices/.pw-symlinks") == "c:\t../drive_c\nz:\t/\n");
  CHECK(!std::filesystem::is_symlink(bottle / "dosdevices/c:"));
  for (const auto& path : {bottle, bottle / "drive_c", bottle / "drive_c/windows", bottle / "dosdevices"}) {
    struct stat info{};
    CHECK(::lstat(path.c_str(), &info) == 0);
    CHECK(info.st_uid == ::geteuid());
    CHECK((info.st_mode & 0777) == 0700);
  }
  struct stat registry{};
  CHECK(::lstat((bottle / "drive_c/windows/system.reg").c_str(), &registry) == 0);
  CHECK(registry.st_uid == ::geteuid());
  CHECK((registry.st_mode & 0777) == 0600);
  // The profile's empty folders exist although the kit has no file under users/ (Unity needs LocalLow).
  for (const char* folder : {"AppData/LocalLow", "AppData/Roaming", "Documents", "Saved Games"}) {
    struct stat profile{};
    CHECK(::lstat((bottle / "drive_c/users/prospero" / folder).c_str(), &profile) == 0 && S_ISDIR(profile.st_mode));
    CHECK((profile.st_mode & 0777) == 0700);
  }
  CHECK(installer.ValidateArtifacts(record).state == PackageActionState::kComplete);
  CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete);
  const auto status = installer.Inspect(record);
  CHECK(status.known && status.title_staged && status.manifest_matches);
  CHECK(status.template_owned && status.mutable_bottle_present && status.mutable_bottle_owned);
  CHECK(status.artifacts_valid && status.profile_published);
  CHECK(status.profile_settings_revision == std::numeric_limits<std::uint64_t>::max());

  // Inspection is evidence-based rather than cached.
  Write(title / "win/wine/lib/core.prx", "corrupt", 0755);
  CHECK(!installer.Inspect(record).artifacts_valid);
  Write(title / "win/wine/lib/core.prx", "runtime", 0644);
  CHECK(!installer.Inspect(record).manifest_matches);

  // Settings updates check ownership only: the owner markers name this install, so
  // drift in the title's files, folder modes or kit is left to the full inspection.
  CHECK(::chmod(title.c_str(), 0700) == 0);
  auto owned = installer.InspectOwnership(record);
  CHECK(owned.known && owned.title_staged && owned.manifest_matches && !owned.artifacts_valid);
  CHECK(owned.template_owned && !owned.profile_published);  // applies prove it from their receipt
  CHECK(::chmod(title.c_str(), 0755) == 0);
  const auto marker = Read(title / ".z3cloudplay-title-owner");
  Write(title / ".z3cloudplay-title-owner", marker + "kit_id=other\n", 0644);
  owned = installer.InspectOwnership(record);
  CHECK(owned.known && owned.manifest_matches && !installer.TitleCurrent(record));
  auto other = record; other.install_id = "another-install";
  Write(title / ".z3cloudplay-title-owner", marker, 0644);
  CHECK(!installer.InspectOwnership(other).manifest_matches);
  std::filesystem::remove(title / ".z3cloudplay-title-owner");
  CHECK(!installer.InspectOwnership(record).manifest_matches);
}

void CancelAndSpaceFailuresAreBounded() {
  Fixture fixture;
  auto record = Record();
  auto installer = fixture.Installer();
  fixture.free_bytes = 1;
  const auto no_space = installer.StageTitle(record);
  CHECK(no_space.state == PackageActionState::kFailed && no_space.code == "space-unavailable");
  CHECK(!std::filesystem::exists(fixture.host_data / "homebrew/FAKE78001"));
  fixture.free_bytes = 1ULL << 30;
  fixture.cancel_after_checks = 2;
  const auto canceled = installer.StageTitle(record);
  CHECK(canceled.state == PackageActionState::kCanceled);
  CHECK(!std::filesystem::exists(fixture.host_data / "homebrew/FAKE78001"));
  CHECK(std::filesystem::exists(fixture.host_data /
                                "z3cloudplay/staging/title-install-hl2/.z3cloudplay-stage-owner"));
  CHECK(!std::filesystem::exists(record.install_path));
  fixture.cancel_after_checks = -1;
  fixture.cancel_checks = 0;
  auto restarted = fixture.Installer();
  CHECK(restarted.StageTitle(record).state == PackageActionState::kComplete);
}

// Where free space cannot be read, only an explicit unverifiable mode copies.
void UnreadableFreeSpaceRequiresExplicitMode() {
  for (const bool unverifiable : {false, true}) {
    Fixture fixture;
    auto record = Record();
    auto operations = PackageInstallerOperations{};
    operations.hash_file = TestHash; operations.hash_bytes = TestHashBytes;
    operations.validate_bottle = [](const std::filesystem::path&) { return true; };
    operations.free_bytes = [](const std::filesystem::path&) { return std::optional<std::uint64_t>{}; };
    operations.free_space_unverifiable = unverifiable;
    FilesystemPackageInstaller installer(fixture.view, fixture.paths, operations);
    const auto staged = installer.StageTitle(record);
    CHECK((staged.state == PackageActionState::kComplete) == unverifiable);
    if (!unverifiable) CHECK(staged.code == "space-unavailable");
  }
}

void SyncFailureNeverPublishesWatchedTitle() {
  Fixture fixture;
  fixture.fail_sync_call = 5;
  auto installer = fixture.Installer();
  const auto failed = installer.StageTitle(Record());
  CHECK(failed.state == PackageActionState::kFailed && failed.code == "sync-failed");
  CHECK(!std::filesystem::exists(fixture.host_data / "homebrew/FAKE78001"));
  CHECK(std::filesystem::exists(fixture.host_data /
                                "z3cloudplay/staging/title-install-hl2/.z3cloudplay-stage-owner"));
  fixture.fail_sync_call = -1;
  fixture.sync_calls = 0;
  auto restarted = fixture.Installer();
  CHECK(restarted.StageTitle(Record()).state == PackageActionState::kComplete);
}

void NativeFsyncFallbackPublishesAtomically() {
  Fixture fixture;
  auto installer = fixture.Installer(false);
  CHECK(installer.StageTitle(Record()).state == PackageActionState::kComplete);
  CHECK(installer.CopyTemplate(Record()).state == PackageActionState::kComplete);
}

// A stop between moving the title into place and publishing its folders leaves
// them private. Inspection reports the title unmatched and staging republishes it
// instead of refusing it as a collision.
// One shell for every game: any managed title but the template's uses it, and a game title
// only ever gets the parameters Z3CloudPlay wrote for it, naming its own ID.
void AnyTitleVariantServesEveryTitle() {
  Fixture fixture;
  const std::string template_param = "{\"titleId\": \"FAKE78000\", \"titleName\": \"Prospero Win\"}\n";
  Write(fixture.kit / "titles/variant/sce_sys/param.json", template_param, 0644);
  for (auto& file : fixture.view.files)
    if (file.path == "titles/variant/sce_sys/param.json") {
      file.size = template_param.size();
      file.sha256 = *TestHash(fixture.kit / file.path);
    }
  fixture.view.variants = {{"any", "FAKE78000", "titles/variant", true}};

  const auto& view = fixture.view;
  CHECK(KitVariantFor(view, "FAKE78001") == &view.variants[0]);
  CHECK(KitVariantFor(view, "FAKE78999") == &view.variants[0]);
  CHECK(KitVariantFor(view, "FAKE78000") == nullptr);  // the template is never a game title
  CHECK(KitVariantFor(view, "PPSA99995") == nullptr && KitVariantFor(view, "FAKE7801") == nullptr);
  auto candidates = KitTitleCandidates(view);
  CHECK(candidates.size() == 999 && candidates.front() == "FAKE78001" && candidates.back() == "FAKE78999");
  CHECK(std::find(candidates.begin(), candidates.end(), "FAKE78000") == candidates.end());

  // A fixed variant keeps its title and is offered first; the any-title shell serves the rest.
  auto mixed = view;
  mixed.variants.push_back({"fixed", "FAKE78005", "titles/variant"});
  CHECK(KitVariantFor(mixed, "FAKE78005") == &mixed.variants[1]);
  CHECK(KitVariantFor(mixed, "FAKE78006") == &mixed.variants[0]);
  candidates = KitTitleCandidates(mixed);
  CHECK(candidates.size() == 999 && candidates[0] == "FAKE78005" && candidates[1] == "FAKE78001");
  // Fixed variants only: exactly their titles, as before.
  auto fixed = view;
  fixed.variants = {{"fixed", "FAKE78005", "titles/variant"}};
  CHECK(KitTitleCandidates(fixed) == std::vector<std::string>{"FAKE78005"});
  CHECK(KitVariantFor(fixed, "FAKE78006") == nullptr);

  auto record = Record();
  record.title_id = "FAKE78012";
  std::filesystem::create_directories(fixture.host_data);
  auto installer = fixture.Installer();
  // Without the game's own parameters the template's would name the wrong title.
  CHECK(installer.StageTitle(record).state != PackageActionState::kComplete);
  CHECK(!std::filesystem::exists(fixture.host_data / "homebrew/FAKE78012"));
  CHECK(installer.WriteTitleMetadata(record, template_param, std::nullopt).state != PackageActionState::kComplete);
  CHECK(installer.WriteTitleMetadata(record, "not json", std::nullopt).state != PackageActionState::kComplete);
  const std::string own = "{\"titleId\": \"FAKE78012\", \"titleName\": \"Half-Life 2\"}\n";
  CHECK(installer.WriteTitleMetadata(record, own, std::nullopt).state == PackageActionState::kComplete);
  CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(Read(fixture.host_data / "homebrew/FAKE78012/sce_sys/param.json") == own);
  CHECK(Read(fixture.host_data / "homebrew/FAKE78012/eboot.bin") == "native");
  CHECK(installer.TitleMetadataCurrent(record) && installer.Inspect(record).manifest_matches);
}

// A game prepared on a fixed kit title moves to the any-title shell when its kit is respun:
// same title, same bottle, the new shell's files.
void FixedTitleRefreshesToAnyTitleShell() {
  Fixture fixture;
  std::filesystem::create_directories(fixture.host_data);
  auto record = Record();
  for (const auto& artifact : fixture.view.artifacts)
    record.artifacts.push_back({artifact.artifact_id, artifact.identity, artifact.sha256});
  const std::string own = "{\"titleId\": \"FAKE78001\", \"titleName\": \"Half-Life 2\"}\n";
  {
    auto installer = fixture.Installer();
    CHECK(installer.WriteTitleMetadata(record, own, std::nullopt).state == PackageActionState::kComplete);
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    CHECK(installer.TitleCurrent(record));
  }
  Write(fixture.kit / "titles/variant/eboot.bin", "any-title-shell", 0755);
  for (auto& file : fixture.view.files)
    if (file.path == "titles/variant/eboot.bin") {
      file.size = std::string("any-title-shell").size();
      file.sha256 = *TestHash(fixture.kit / file.path);
    }
  // The new shell also brings a folder the title did not have.
  const std::string zink = "titles/variant/win/mesa-zink/i386-windows/opengl32.dll";
  Write(fixture.kit / zink, "MZ zink", 0644);
  fixture.view.files.push_back({zink, std::string("MZ zink").size(), *TestHash(fixture.kit / zink), 0644});
  fixture.view.files_sha256 = std::string(64, 'b');
  fixture.view.variants = {{"any-title", "FAKE78000", "titles/variant", true}};
  auto respun = fixture.Installer();
  CHECK(!respun.TitleCurrent(record));
  // The new kit does not make the title someone else's: settings still apply to it,
  // while the full inspection (launch) waits for Update (#5).
  const auto owned = respun.InspectOwnership(record);
  CHECK(owned.known && owned.manifest_matches && owned.template_owned && owned.mutable_bottle_owned);
  CHECK(!respun.Inspect(record).manifest_matches);
  fixture.progress.clear();
  const auto refreshed = respun.RefreshTitle(record);
  // Update shows how far the refresh is: it reports up to the title's whole size.
  CHECK(fixture.progress.size() >= 2 && fixture.progress.front().first == 0 &&
        fixture.progress.back().first == fixture.progress.back().second && fixture.progress.back().second > 0);
  if (refreshed.state != PackageActionState::kComplete) std::fprintf(stderr, "refresh: %s %s\n", refreshed.code.c_str(), refreshed.message.c_str());
  CHECK(refreshed.state == PackageActionState::kComplete);
  CHECK(respun.TitleCurrent(record));
  CHECK(Read(fixture.host_data / "homebrew/FAKE78001/eboot.bin") == "any-title-shell");
  CHECK(Read(fixture.host_data / "homebrew/FAKE78001/sce_sys/param.json") == own);
  CHECK(Read(fixture.host_data / "homebrew/FAKE78001/win/mesa-zink/i386-windows/opengl32.dll") == "MZ zink");
  struct stat folder{};
  CHECK(::stat((fixture.host_data / "homebrew/FAKE78001/win/mesa-zink").c_str(), &folder) == 0 &&
        (folder.st_mode & 07777) == 0755);
}

void PrivateTitleFoldersAreRepublished() {
  Fixture fixture;
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(Record()).state == PackageActionState::kComplete);
  const auto title = fixture.host_data / "homebrew/FAKE78001";
  CHECK(::chmod(title.c_str(), 0700) == 0);
  CHECK(!installer.Inspect(Record()).manifest_matches);
  CHECK(installer.StageTitle(Record()).state == PackageActionState::kComplete);
  struct stat info{};
  CHECK(::stat(title.c_str(), &info) == 0 && (info.st_mode & 07777) == 0755);
  CHECK(installer.Inspect(Record()).manifest_matches);
}

// Console titles run with umask 0: folders the installer creates still get an
// explicit mode that others cannot write.
void CreatedFoldersIgnoreUmask() {
  const auto previous = ::umask(0);
  Fixture fixture;
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(Record()).state == PackageActionState::kComplete);
  ::umask(previous);
  struct stat info{};
  CHECK(::stat((fixture.host_data / "z3cloudplay/staging").c_str(), &info) == 0 && (info.st_mode & 07777) == 0755);
}

// A staging folder left empty by a stop before its owner marker is reclaimed; one
// with content and no marker is still refused.
void EmptyUnmarkedStagingIsReclaimed() {
  Fixture fixture;
  auto installer = fixture.Installer();
  const auto staging = fixture.host_data / ("z3cloudplay/staging/title-" + Record().install_id);
  std::filesystem::create_directories(staging);
  CHECK(installer.StageTitle(Record()).state == PackageActionState::kComplete);

  Fixture stray;
  auto refusing = stray.Installer();
  const auto stray_staging = stray.host_data / ("z3cloudplay/staging/title-" + Record().install_id);
  Write(stray_staging / "unknown", "not ours", 0600);
  CHECK(refusing.StageTitle(Record()).code == "staging-collision");
  CHECK(std::filesystem::exists(stray_staging / "unknown"));
}

void LateSyncFailureIsReestablishedOnRetry() {
  Fixture fixture;
  fixture.fail_title_post_rename_once = true;
  auto installer = fixture.Installer();
  CHECK(installer.StageTitle(Record()).code == "sync-failed");
  CHECK(std::filesystem::is_directory(fixture.host_data / "homebrew/FAKE78001"));
  CHECK(installer.StageTitle(Record()).state == PackageActionState::kComplete);

  fixture.fail_bottle_post_rename_once = true;
  CHECK(installer.CopyTemplate(Record()).code == "sync-failed");
  CHECK(std::filesystem::is_directory(
      fixture.host_data / "z3cloudplay/games/install-hl2/library/prefixes/game"));
  CHECK(installer.CopyTemplate(Record()).state == PackageActionState::kComplete);
}

void CollisionsTraversalSymlinksAndModesFailClosed() {
  {
    Fixture fixture;
    auto installer = fixture.Installer();
    Write(fixture.host_data / "homebrew/FAKE78001/foreign.txt", "keep");
    CHECK(installer.StageTitle(Record()).code == "title-collision");
    CHECK(Read(fixture.host_data / "homebrew/FAKE78001/foreign.txt") == "keep");
  }
  {
    Fixture fixture;
    fixture.view.files[0].path = "titles/variant/../escape";
    auto installer = fixture.Installer();
    CHECK(!installer.Inspect(Record()).known);
    CHECK(installer.StageTitle(Record()).code == "kit-invalid");
  }
  {
    Fixture fixture;
    std::filesystem::remove(fixture.kit / "titles/variant/eboot.bin");
    std::filesystem::create_symlink("/etc/passwd",
                                    fixture.kit / "titles/variant/eboot.bin");
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(Record()).code == "kit-source-invalid");
  }
  {
    Fixture fixture;
    ::chmod((fixture.kit / "titles/variant/eboot.bin").c_str(), 0644);
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(Record()).code == "kit-source-invalid");
  }
  {
    Fixture fixture;
    std::filesystem::create_directories(fixture.host_data / "homebrew");
    std::filesystem::create_directories(fixture.temporary.path / "foreign-title");
    Write(fixture.temporary.path / "foreign-title/keep", "foreign");
    std::filesystem::create_directory_symlink(fixture.temporary.path / "foreign-title",
                                              fixture.host_data / "homebrew/FAKE78001");
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(Record()).code == "kit-invalid");
    CHECK(Read(fixture.temporary.path / "foreign-title/keep") == "foreign");
  }
}

void RestartDoesNotRecopyOrDestroyMutableFiles() {
  Fixture fixture;
  auto record = Record();
  auto first = fixture.Installer();
  CHECK(first.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(first.CopyTemplate(record).state == PackageActionState::kComplete);
  const auto save = fixture.host_data /
                    "z3cloudplay/games/install-hl2/library/prefixes/game/drive_c/save.dat";
  Write(save, "player-save");

  auto restarted = fixture.Installer();
  CHECK(restarted.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(restarted.CopyTemplate(record).state == PackageActionState::kComplete);
  CHECK(Read(save) == "player-save");

  std::filesystem::remove(fixture.host_data /
                          "z3cloudplay/games/install-hl2/library/prefixes/game/.z3cloudplay-template-owner");
  CHECK(restarted.CopyTemplate(record).code == "bottle-collision");
  CHECK(Read(save) == "player-save");
}

void CorruptPublicationAndInvalidDriveTableAreRejected() {
  {
    Fixture fixture;
    auto record = Record();
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete);
    Write(fixture.host_data / "z3cloudplay/games/install-hl2/library/profiles/game.profile",
          "changed");
    CHECK(!installer.Inspect(record).profile_published);
  }
  {
    Fixture fixture;
    auto record = Record();
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    Write(fixture.host_data / "homebrew/FAKE78001/game-launch.json", "foreign");
    CHECK(installer.PublishProfile(record).code == "publication-collision");
    CHECK(Read(fixture.host_data / "homebrew/FAKE78001/game-launch.json") == "foreign");
  }
  {
    Fixture fixture;
    Write(fixture.kit / fixture.view.template_drive_table,
          "c:\t../drive_c\nz:\t/\ny:\t../../escape\n");
    fixture.view.files[4].size =
        std::filesystem::file_size(fixture.kit / fixture.view.template_drive_table);
    fixture.view.files[4].sha256 = *TestHash(fixture.kit / fixture.view.template_drive_table);
    auto installer = fixture.Installer();
    CHECK(installer.CopyTemplate(Record()).code == "template-links-invalid");
  }
}

void InputPresetsAreOwnerPublishedAndHashVerified() {
  Fixture fixture;
  auto record = Record();
  record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                      {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
  auto publication = fixture.Publication(record);
  publication.input_presets.push_back({"controller.input", "[input]\nmode=xinput\n"});
  publication.required_input_presets.push_back("controller.input");
  for (unsigned i = 0; i < 63; ++i) {
    const auto name = "extra-controller-" + std::to_string(i) + ".input";
    publication.input_presets.push_back({name, "[input]\nmode=keyboard\n"}); publication.required_input_presets.push_back(name);
  }
  PackageInstallerOperations operations;
  operations.hash_file = TestHash;
  operations.hash_bytes = TestHashBytes;
  operations.free_bytes = [](const std::filesystem::path&) {
    return std::optional<std::uint64_t>(1ULL << 30);
  };
  operations.sync_path = [](const std::filesystem::path&, bool) { return true; };
  operations.canceled = [] { return false; };
  operations.validate_bottle = [](const std::filesystem::path& bottle) {
    return std::filesystem::is_regular_file(bottle / "drive_c/windows/system.reg");
  };
  operations.provide_publication = [&](const RuntimeInstall&) {
    return std::optional<ProfilePublication>(publication);
  };
  operations.publish_owned = [&](const RuntimeInstall& r, const PublicationPaths& output, const ProfilePublication& value) {
    return WriteCompleteFixture(fixture.host_data, fixture.view.kit_id, r, output, value);
  };
  FilesystemPackageInstaller installer(fixture.view, fixture.paths, std::move(operations));
  CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
  CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
  CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete);
  const auto preset = fixture.host_data /
                      "z3cloudplay/games/install-hl2/library/input/controller.input";
  CHECK(Read(preset) == "[input]\nmode=xinput\n");
  CHECK(installer.Inspect(record).profile_published);
  CHECK(Read(fixture.host_data / "z3cloudplay/games/install-hl2/library/profiles/.z3cloudplay-publication").size() > 4096);
  Write(preset, "corrupt");
  CHECK(!installer.Inspect(record).profile_published);
}


void MissingReferencedInputAndSymlinkedManagedParentsAreRejected() {
  {
    Fixture fixture;
    auto record = Record();
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    auto publication = fixture.Publication(record);
    publication.required_input_presets.push_back("missing.input");
    PackageInstallerOperations operations;
    operations.hash_file = TestHash;
    operations.hash_bytes = TestHashBytes;
    operations.free_bytes = [](const std::filesystem::path&) {
      return std::optional<std::uint64_t>(1ULL << 30);
    };
    operations.sync_path = [](const std::filesystem::path&, bool) { return true; };
    operations.canceled = [] { return false; };
    operations.validate_bottle = [](const std::filesystem::path& bottle) {
      return std::filesystem::is_regular_file(bottle / "drive_c/windows/system.reg");
    };
    operations.provide_publication = [&](const RuntimeInstall&) {
      return std::optional<ProfilePublication>(publication);
    };
    operations.publish_owned = [](const RuntimeInstall&, const PublicationPaths&, const ProfilePublication&) {
      return PackageActionResult{PackageActionState::kComplete, {}, {}};
    };
    FilesystemPackageInstaller installer(fixture.view, fixture.paths, std::move(operations));
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    CHECK(installer.PublishProfile(record).code == "publication-invalid");
  }
  {
    Fixture fixture;
    auto record = Record();
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    const auto library = fixture.host_data / "z3cloudplay/games/install-hl2/library";
    std::filesystem::remove_all(library / "profiles");
    std::filesystem::create_directories(fixture.temporary.path / "outside");
    std::filesystem::create_directory_symlink(fixture.temporary.path / "outside",
                                              library / "profiles");
    CHECK(!installer.Inspect(record).known);
    CHECK(installer.PublishProfile(record).code == "kit-invalid");
    CHECK(std::filesystem::is_empty(fixture.temporary.path / "outside"));
  }
}

}  // namespace

int main() {
  TemplateCopyReportsProgress();
  {
    Fixture fixture;
    const auto game = fixture.host_data / "z3cloudplay/games/install-hl2";
    std::filesystem::create_directories(game);
    CHECK(::chmod(game.c_str(), 0755) == 0);
    Write(game / "existing-save", "keep this save");
    std::string detail;
    CHECK(!EnsurePrivateDirectories(fixture.host_data, game, game / "library/prefixes", &detail));
    CHECK(detail.find("wrong owner or permissions") != std::string::npos);
    CHECK(Read(game / "existing-save") == "keep this save");
    struct stat info{};
    CHECK(::stat(game.c_str(), &info) == 0 && (info.st_mode & 0777) == 0755);
    CHECK(!std::filesystem::exists(game / "library"));
    CHECK(!EnsurePrivateDirectories(fixture.host_data, game, fixture.host_data / "../outside", &detail));
  }
  {
    Fixture fixture;
    const auto game = fixture.host_data / "z3cloudplay/games/install-hl2";
    std::filesystem::create_directories(game.parent_path());
    std::filesystem::create_directories(fixture.temporary.path / "outside");
    std::filesystem::create_directory_symlink(fixture.temporary.path / "outside", game);
    std::string detail;
    CHECK(!EnsurePrivateDirectories(fixture.host_data, game, game / "library", &detail));
    CHECK(std::filesystem::is_empty(fixture.temporary.path / "outside"));
  }
  for (const auto& relative : {"library", "library/prefixes"}) {
    Fixture fixture;
    auto installer = fixture.Installer();
    const auto record = Record();
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    const auto drifted = fixture.host_data / "z3cloudplay/games/install-hl2" / relative;
    CHECK(::chmod(drifted.c_str(), 0755) == 0);
    CHECK(!installer.Inspect(record).mutable_bottle_owned);
    CHECK(installer.CopyTemplate(record).code == "bottle-collision");
    CHECK(Read(fixture.host_data / "z3cloudplay/games/install-hl2/library/prefixes/game/drive_c/windows/system.reg") == "registry");
  }
  {
    Fixture fixture;
    auto installer = fixture.Installer();
    struct rlimit original{}, limited{};
    CHECK(::getrlimit(RLIMIT_FSIZE, &original) == 0);
    limited = original;
    limited.rlim_cur = 8; // Exercise a partial stage marker write, before payload copying.
    const auto previous = std::signal(SIGXFSZ, SIG_IGN);
    CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const auto result = installer.CopyTemplate(Record());
    CHECK(::setrlimit(RLIMIT_FSIZE, &original) == 0);
    std::signal(SIGXFSZ, previous);
    CHECK(result.code == "staging-unavailable");
    CHECK(!std::filesystem::exists(fixture.host_data / "z3cloudplay/staging/bottle-install-hl2"));
    CHECK(installer.CopyTemplate(Record()).state == PackageActionState::kComplete);
  }
  {
    Fixture fixture;
    auto record = Record();
    record.artifacts = {{"runtime", "rev", fixture.view.artifacts[0].sha256},
                        {"recipe", "steam-220-hl2-v1-1", fixture.view.artifacts[1].sha256}};
    auto installer = fixture.Installer();
    CHECK(installer.StageTitle(record).state == PackageActionState::kComplete);
    CHECK(installer.CopyTemplate(record).state == PackageActionState::kComplete);
    fixture.publish_public_profile = true;
    CHECK(installer.PublishProfile(record).code == "publication-verification");
    CHECK(!installer.Inspect(record).profile_published);
    const auto profiles = fixture.host_data / "z3cloudplay/games/install-hl2/library/profiles";
    CHECK(IsPrivateDirectory(profiles));
    fixture.publish_public_profile = false;
    CHECK(installer.PublishProfile(record).state == PackageActionState::kComplete);
    CHECK(::chmod(profiles.c_str(), 0755) == 0);
    CHECK(!installer.Inspect(record).profile_published);
    CHECK(installer.PublishProfile(record).code == "private-directory");
  }
#ifdef Z3CLOUDPLAY_TEST_SETTINGS_OWNER_INTEGRATION
  RealFilesystemInstallerAndOwnerRecoverPendingPublication();
#endif
  FailedDriverCreatesNoPublicationDirectories();
  CompleteInspectorRefusesMixedSettingsAndBusyPublisher();
  CommittedReceiptIsRequired();
  LegacyDescriptorOnlyBindsHistoricalRoot();
  CompletePublicationAndFreshInspection();
  CancelAndSpaceFailuresAreBounded();
  UnreadableFreeSpaceRequiresExplicitMode();
  SyncFailureNeverPublishesWatchedTitle();
  NativeFsyncFallbackPublishesAtomically();
  LateSyncFailureIsReestablishedOnRetry();
  PrivateTitleFoldersAreRepublished();
  AnyTitleVariantServesEveryTitle();
  FixedTitleRefreshesToAnyTitleShell();
  CreatedFoldersIgnoreUmask();
  EmptyUnmarkedStagingIsReclaimed();
  CollisionsTraversalSymlinksAndModesFailClosed();
  RestartDoesNotRecopyOrDestroyMutableFiles();
  CorruptPublicationAndInvalidDriveTableAreRejected();
  InputPresetsAreOwnerPublishedAndHashVerified();
  MissingReferencedInputAndSymlinkedManagedParentsAreRejected();
  std::puts("runtime package installer tests passed");
  return 0;
}
