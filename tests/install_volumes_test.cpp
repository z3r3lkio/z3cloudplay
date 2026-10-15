#include "steam/install_volumes.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <unistd.h>

using namespace z3cloudplay::steam;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

int main() {
  const auto base = std::filesystem::temp_directory_path() / ("z3cloudplay-volumes-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "app0");
  std::filesystem::create_directories(base / "ext1");
  std::filesystem::create_directories(base / "locked");

  const std::vector<VolumeCandidate> candidates = {
      {"internal", "Console storage", (base / "app0").string(), (base / "app0" / "games").string()},
      {"m2", "M.2 SSD", (base / "ext1").string(), (base / "ext1" / "z3cloudplay" / "games").string()},
      {"usb0", "USB drive", (base / "absent").string(), (base / "absent" / "games").string()},
      {"locked", "Read-only drive", (base / "locked").string(), (base / "locked" / "z3cloudplay" / "games").string()},
  };
  std::filesystem::permissions(base / "locked", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

  const auto volumes = DiscoverInstallVolumes(candidates);
  // The primary is always offered; a missing mount is skipped; an unwritable one is offered unavailable.
  CHECK(volumes.size() == 3);
  CHECK(volumes[0].id == "internal" && volumes[0].available);
  CHECK(volumes[1].id == "m2" && volumes[1].available);
  CHECK(std::filesystem::is_directory(base / "ext1" / "z3cloudplay" / "games"));
  CHECK(!std::filesystem::exists(base / "ext1" / "z3cloudplay" / "games" / ".z3cloudplay-probe"));
  CHECK(volumes[2].id == "locked");
  if (::geteuid() != 0) CHECK(!volumes[2].available);  // root ignores the permission bits

  // A folder on the same filesystem as its parent is an empty mount point, not a drive:
  // it is skipped and nothing is created inside it.
  std::filesystem::create_directories(base / "unmounted");
  const auto unmounted = DiscoverInstallVolumes({
      candidates[0],
      {"extended", "Extended storage", (base / "unmounted").string(), (base / "unmounted" / "z3cloudplay" / "games").string(), true},
  });
  CHECK(unmounted.size() == 1 && unmounted[0].id == "internal");
  CHECK(!std::filesystem::exists(base / "unmounted" / "z3cloudplay"));
  // The filesystem root is a mount point on any host, so a required mount point there is offered.
  const auto mounted = DiscoverInstallVolumes({candidates[0], {"root", "Root", "/", (base / "root-games").string(), true}});
  CHECK(mounted.size() == 2 && mounted[1].id == "root");

  // The console candidates start with console storage under /app0; drives must be mounted.
  const auto console = ConsoleVolumeCandidates();
  CHECK(!console.empty() && console[0].id == InstallService::kPrimaryVolumeId && console[0].games_root == "/app0/games");
  CHECK(!console[0].require_mount_point);
  for (std::size_t index = 1; index < console.size(); ++index) CHECK(console[index].require_mount_point);

  const auto space = QueryVolumeSpace(base.string());
  CHECK(space && space->total_bytes > 0 && space->free_bytes <= space->total_bytes);
  // A new game's directories may not exist yet; query the existing volume ancestor.
  const auto missing = QueryVolumeSpace((base / "absent" / "new-game").string());
  CHECK(missing && missing->total_bytes == space->total_bytes);
  CHECK(!std::filesystem::exists(base / "absent"));
  CHECK(!QueryVolumeSpace(""));
  CHECK(QueryVolumeSpace(base.string() + "//./"));
  CHECK(!QueryVolumeSpace(base.string() + "/../games"));
  CHECK(QueryVolumeSpace("."));
  CHECK(!QueryVolumeSpace(std::string("/tmp\0ignored", 12)));
  CHECK(!QueryVolumeSpace("/z3cloudplay-unknown-volume-" + std::to_string(::getpid()) + "/games"));
  std::FILE* regular = std::fopen((base / "file").c_str(), "w");
  CHECK(regular && std::fclose(regular) == 0);
  CHECK(!QueryVolumeSpace((base / "file" / "games").string()));

  const auto converted = detail::VolumeSpaceFromBlocks(65536, 15261605, 3799429);
  CHECK(converted && converted->total_bytes == 1000184545280ull && converted->free_bytes == 248999378944ull);
  const auto reserved = detail::VolumeSpaceFromBlocks(4096, 100, -1);
  CHECK(reserved && reserved->total_bytes == 409600 && reserved->free_bytes == 0);
  CHECK(!detail::VolumeSpaceFromBlocks(0, 100, 10));
  CHECK(!detail::VolumeSpaceFromBlocks(-1, 100, 10));
  CHECK(!detail::VolumeSpaceFromBlocks(4096, 0, 0));
  CHECK(!detail::VolumeSpaceFromBlocks(4096, 10, 11));
  CHECK(!detail::VolumeSpaceFromBlocks(4096, std::numeric_limits<std::uint64_t>::max(), 1));
  for (const auto* root : {"/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1"}) {
    CHECK(detail::VolumeMountMatches(root, root));
    CHECK(detail::VolumeMountMatches(std::string(root) + "/z3cloudplay/games/new-game", root));
    CHECK(!detail::VolumeMountMatches(root, "/mnt"));
    CHECK(!detail::VolumeMountMatches(std::string(root) + "/z3cloudplay", "/mnt"));
    CHECK(!detail::VolumeMountMatches(root, "/mnt/ext2"));
  }
  CHECK(detail::VolumeMountMatches("/data/z3cloudplay/games", "/data"));
  CHECK(detail::VolumeMountMatches("/mnt/ext10", "/mnt"));

  std::filesystem::permissions(base / "locked", std::filesystem::perms::owner_all);
  std::filesystem::remove_all(base);
  std::puts("install volumes tests passed");
}
