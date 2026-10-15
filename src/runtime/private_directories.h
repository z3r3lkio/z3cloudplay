#pragma once
#include <filesystem>
#include <string>
namespace playden::runtime {
// Creates directories as the current effective user, without credential changes.
// boundary is an existing trusted directory; private_root and descendants must
// belong to that user and have mode 0700. Existing foreign/public directories
// in the private tree are rejected, never chmodded or taken over.
bool EnsurePrivateDirectories(const std::filesystem::path& boundary,
                              const std::filesystem::path& private_root,
                              const std::filesystem::path& directory,
                              std::string* error);
bool IsPrivateDirectoryTree(const std::filesystem::path& private_root,
                            const std::filesystem::path& directory);
bool IsPrivateDirectory(const std::filesystem::path& directory);
}
