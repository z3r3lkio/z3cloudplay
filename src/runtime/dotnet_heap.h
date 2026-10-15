#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace playden::runtime {

// .NET 7 and later reserve 256 GiB of address space for the garbage collector's regions at
// start, which the console refuses ("GC heap initialization failed", results/20261008-slay-
// the-spire-2). A hard heap limit shrinks that reservation. Games that ship their own CoreCLR
// (coreclr.dll beside a *.runtimeconfig.json, as Godot .NET exports do) get the limit written
// into the runtimeconfig's configProperties unless they set one themselves. Idempotent; the
// game must be stopped. Links are never followed.
inline constexpr std::uint64_t kDotnetHeapHardLimit = std::uint64_t{2} << 30;

struct DotnetHeapResult {
  bool ok = true;
  std::string message;               // on failure: a sentence for the player
  std::vector<std::string> changed;  // runtimeconfig files (relative) given the limit
  std::vector<std::string> found;    // runtimeconfig files (relative) beside a coreclr.dll
};

DotnetHeapResult LimitDotnetHeap(const std::string& game_root);

}  // namespace playden::runtime
