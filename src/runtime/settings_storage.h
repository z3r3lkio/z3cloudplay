#pragma once
#include <filesystem>
#include <optional>
#include <string>
namespace playden::runtime {
// Select an existing absolute, normalized app-state root (or one whose parent
// exists). Keep its public mode, create a private settings child, import the
// legacy owned file once, and validate the selected record before enabling
// settings writes. Failure preserves both sources for explicit recovery.
std::optional<std::filesystem::path> PrepareGameSettingsStorage(
    const std::filesystem::path& state_root, std::string* reason);
}
