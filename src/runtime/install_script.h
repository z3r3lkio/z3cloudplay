#pragma once

#include "runtime/game_settings.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// A Steam install script (installscript.vdf in the game folder): Steam applies its Registry
// section on a game's first start, and some games look for those keys (Fallout: New Vegas's
// launcher offers only "Install" without HKLM\SOFTWARE\Bethesda Softworks\FalloutNV
// "Installed Path"). Playden writes them into the bottle instead. Run Process entries
// (redistributable installers) are not run: Wine's built-ins stand in for them.

// The values a script's Registry section sets, by hive. %INSTALLDIR% becomes install_dir (the
// game folder's Windows path). HKLM\Software keys go to both registry views, as Steam, a
// 32-bit program, writes them where 32-bit and 64-bit games find them. Only "string" and
// "dword" values are taken; a value with another %TOKEN% or non-ASCII text, and keys under
// other roots, are skipped. Text may be UTF-8 or UTF-16 with a byte order mark.
struct InstallScriptValues {
  std::vector<RegistryValue> machine;  // system.reg (HKEY_LOCAL_MACHINE)
  std::vector<RegistryValue> user;     // user.reg (HKEY_CURRENT_USER)
};
bool ParseInstallScript(std::string_view text, const std::string& install_dir, InstallScriptValues* values,
                        std::string* reason);

// Applies the Registry sections of the install scripts at the top of game_root (file names
// starting with "installscript" and ending ".vdf", any case) to the stopped bottle at
// prefix_dir, backing each hive up first. An unchanged hive is not written. found lists the
// scripts read; a script that cannot be read or parsed is skipped and named in skipped.
struct InstallScriptResult {
  std::vector<std::string> found;
  std::vector<std::string> skipped;
  std::size_t values = 0;
  bool ok = true;
  std::string message;  // on failure
};
InstallScriptResult ApplyInstallScripts(const std::string& game_root, const std::string& install_dir,
                                        const std::string& prefix_dir, std::int64_t unix_time);

}  // namespace playden::runtime
