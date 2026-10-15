#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace playden::runtime {

// SteamStub is Steam's DRM wrapper: the program starts in a ".bind" section that checks for
// the Steam client, decrypts the code section and jumps to the original entry point. The
// console has no Steam client, so Playden restores the program as it was built: code
// decrypted, entry point back, ".bind" removed when nothing else needs it. Variants 3.1 and 3.0
// are handled in 64-bit and 32-bit programs (also with the code left unencrypted, and when the
// wrapper starts from a TLS callback), variants 2.1, 2.0 and 1.0 in 32-bit ones; others are refused.

enum class SteamStubKind {
  kNone,         // not wrapped (or not a program)
  kSupported,    // wrapped in a variant Playden unpacks
  kUnsupported,  // wrapped, but in a variant or with options Playden doesn't handle
};

struct SteamStubInfo {
  SteamStubKind kind = SteamStubKind::kNone;
  std::string detail;  // for logs: "3.1 x64", "3.0 x86 (TLS)", "2.1 x86"..., or why it is unsupported
  std::uint32_t app_id = 0;  // the Steam app the wrapper checks for
};

// From the headers of the program at path only (a few KB); links are not followed.
SteamStubInfo InspectSteamStub(const std::string& path);

// Unpacks a whole program in place. False (with why, for logs) when it is not a supported
// SteamStub program; program is then unchanged.
bool UnpackSteamStub(std::string* program, std::string* why);

// Largest program Playden unpacks (it is held in memory while it is rewritten).
inline constexpr std::size_t kSteamStubMaxProgramBytes = 512u * 1024u * 1024u;

struct SteamStubResult {
  bool ok = false;
  std::string message;               // on failure: a sentence for the player
  std::vector<std::string> changed;  // programs (relative) unpacked by this call
  std::vector<std::string> found;    // programs (relative) still or formerly wrapped
};

// Unpacks each wrapped program (relative to install_root, '/' separators) and keeps the
// original as <program>.orig, as the Steamworks swap does: idempotent, run while the game is
// stopped; a newer wrapped program from a game update becomes the new .orig. A program that
// is already unpacked and has its .orig counts as found.
SteamStubResult UnpackSteamStubPrograms(const std::string& install_root, const std::vector<std::string>& programs);

}  // namespace playden::runtime
