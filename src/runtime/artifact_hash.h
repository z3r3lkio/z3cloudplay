#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace playden::runtime {

std::optional<std::string> HashArtifactBytes(std::string_view bytes);
// Streams ordinary files through a descriptor; rejects symlinks in every path
// component and special files. Failure never returns a partial digest.
std::optional<std::string> HashArtifactFile(const std::filesystem::path& path);

}  // namespace playden::runtime
