#pragma once
// uninstall_flow.hpp - The order of an uninstall: Cloud saves, then the PS5 title, then the download.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include "homebrew/simulation.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace z3cloudplay::homebrew {
// Uninstall keeps saves safe and never leaves a PS5 title pointing at deleted files:
// Steam Cloud syncs first (a conflict or a failure stops and asks the player), then
// the game's PS5 title is removed (journaled by the runtime host), and only then the
// Steam download. A failure stops the uninstall with the reason; nothing is skipped.
enum class UninstallStep { kSync, kRemoveTitle, kRemoveDownload };

struct UninstallAction {
  enum class Kind { kNone, kRemoveTitle, kRemoveDownload, kStop } kind = Kind::kNone;
  bool delete_saves = false;
  std::string reason;  // kStop: what the player is told
};

// What the runtime reports about one game right now.
struct UninstallFacts {
  bool has_title = false;            // an install record exists (Ready, failed or being removed)
  bool removing = false;             // the runtime is removing it now
  std::uint64_t removals_handled = 0;
  std::string removal_failure;       // the runtime's latest removal refusal or failure
  bool saves_moving = false;         // a Cloud transfer runs for the game
  bool session_active = false;       // the game's title may be running
};

class UninstallFlow {
 public:
  // Starts an uninstall; false when one already runs for the game. With Cloud the
  // saves sync first, unless the player chose to go on after a failed sync.
  bool Begin(GameId id, bool delete_saves, bool cloud_available);
  // Games whose Cloud sync should be issued now (each once).
  std::vector<GameId> TakeSyncRequests();
  bool WaitingForCloud(GameId id) const;
  // The sync went to the Cloud service: from now on its results settle the uninstall
  // (an older status seen before that is not this sync's result).
  void SyncDispatched(GameId id);
  // The Cloud check finished: continue, or stop and ask.
  UninstallAction CloudSettled(GameId id, CloudStatus status);
  // The Cloud check could not run because the title may be running.
  UninstallAction CloudRefused(GameId id);
  // The game has nothing Cloud could sync (no folder): continue.
  void CloudSkipped(GameId id);
  // What to do next for a game, given what the runtime reports now.
  UninstallAction Advance(GameId id, const UninstallFacts& facts);
  std::optional<UninstallStep> Step(GameId id) const;
  std::vector<GameId> Active() const;

 private:
  struct Entry {
    UninstallStep step = UninstallStep::kSync;
    bool delete_saves = false;
    bool sync_issued = false;
    bool sync_dispatched = false;
    bool remove_sent = false;
    std::uint64_t removals_before = 0;
  };
  UninstallAction Stop(GameId id, std::string reason);
  std::map<GameId, Entry> entries_;
  std::set<GameId> without_sync_;
};
}  // namespace z3cloudplay::homebrew
