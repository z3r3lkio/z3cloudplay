#include "runtime/settings_storage.h"
#include "runtime/game_settings.h"
#include "runtime/private_directories.h"
#include "platform/managed_directory.h"
#include <cerrno>
namespace playden::runtime {
std::optional<std::filesystem::path> PrepareGameSettingsStorage(
    const std::filesystem::path& state_root, std::string* reason) {
  if (reason) reason->clear();
  const auto fail = [&](const char* message) -> std::optional<std::filesystem::path> {
    if (reason) *reason = message;
    return std::nullopt;
  };
  if (!state_root.is_absolute() || state_root.lexically_normal() != state_root)
    return fail("App state root must be an absolute normalized path.");
  const auto private_root = state_root / "game-settings-private";
  const auto path = private_root / "game-settings.json";
  if (!EnsurePrivateDirectories(state_root.parent_path(), private_root, private_root, reason))
    return std::nullopt;
  auto directory = platform::ManagedDirectory::Open(private_root, platform::DirectoryPolicy::kPrivate);
  if (!directory) return fail("Private settings root is unavailable.");
  struct stat existing{};
  const bool present = directory->InspectChild("game-settings.json", &existing);
  if (!present) {
    if (errno != ENOENT) return fail("Private settings file cannot be inspected.");
    if (!MigrateLegacyGameSettingsFile((state_root / "game-settings.json").string(), path.string(), reason))
      return std::nullopt;
  }
  // An existing private file is authoritative. Never fall back to stale legacy
  // bytes when it is malformed or unsafe, and never enable save on that failure.
  std::map<std::uint32_t, GameSettingsRecord> records;
  if (!LoadGameSettingsFile(path.string(), &records, reason)) return std::nullopt;
  return path;
}
}
