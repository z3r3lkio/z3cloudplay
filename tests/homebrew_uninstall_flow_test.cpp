// The order of an uninstall: Cloud saves, then the PS5 title, then the download.
#include "homebrew/uninstall_flow.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using namespace z3cloudplay::homebrew;
using Kind = UninstallAction::Kind;

int main() {
  // With Cloud: the sync is issued once, and nothing is removed until it settles.
  UninstallFlow flow;
  CHECK(flow.Begin(42, true, true));
  CHECK(!flow.Begin(42, false, true));
  CHECK(flow.Step(42) == UninstallStep::kSync);
  UninstallFacts titled;
  titled.has_title = true;
  CHECK(flow.Advance(42, titled).kind == Kind::kNone);
  CHECK(flow.TakeSyncRequests() == std::vector<GameId>{42});
  CHECK(flow.TakeSyncRequests().empty());
  // A status from before the sync was sent is not its result.
  CHECK(flow.CloudSettled(42, CloudStatus::Synced).kind == Kind::kNone && flow.Step(42) == UninstallStep::kSync);
  flow.SyncDispatched(42);
  CHECK(flow.CloudSettled(42, CloudStatus::Syncing).kind == Kind::kNone && flow.WaitingForCloud(42));
  CHECK(flow.CloudSettled(42, CloudStatus::Synced).kind == Kind::kNone);
  CHECK(flow.Step(42) == UninstallStep::kRemoveTitle);

  // The title is removed when no saves move and the game is not running, with the saves choice.
  titled.saves_moving = true;
  CHECK(flow.Advance(42, titled).kind == Kind::kNone);
  titled.saves_moving = false;
  titled.session_active = true;
  CHECK(flow.Advance(42, titled).kind == Kind::kNone);
  titled.session_active = false;
  titled.removals_handled = 7;
  auto action = flow.Advance(42, titled);
  CHECK(action.kind == Kind::kRemoveTitle && action.delete_saves);
  titled.removing = true;
  CHECK(flow.Advance(42, titled).kind == Kind::kNone);
  // Only after the runtime finished and the record is gone does the download go.
  UninstallFacts gone;
  gone.removals_handled = 8;
  CHECK(flow.Advance(42, gone).kind == Kind::kRemoveDownload);
  CHECK(!flow.Step(42) && flow.Active().empty());

  // A failed removal stops with the runtime's reason and keeps the download.
  CHECK(flow.Begin(43, false, false));
  CHECK(flow.TakeSyncRequests().empty());
  titled = {};
  titled.has_title = true;
  CHECK(flow.Advance(43, titled).kind == Kind::kRemoveTitle);
  titled.removals_handled = 1;
  titled.removal_failure = "Close the game before uninstalling it.";
  action = flow.Advance(43, titled);
  CHECK(action.kind == Kind::kStop && action.reason.find("Close the game") != std::string::npos);
  CHECK(!flow.Step(43));

  // A game without a PS5 title goes straight to the download.
  CHECK(flow.Begin(44, false, false));
  CHECK(flow.Advance(44, UninstallFacts{}).kind == Kind::kRemoveDownload);

  // A Cloud conflict stops and asks; uninstalling again syncs again.
  CHECK(flow.Begin(45, false, true));
  flow.TakeSyncRequests();
  flow.SyncDispatched(45);
  action = flow.CloudSettled(45, CloudStatus::Conflict);
  CHECK(action.kind == Kind::kStop && action.reason.find("Choose which Cloud saves") != std::string::npos);
  CHECK(flow.Begin(45, false, true) && flow.Step(45) == UninstallStep::kSync);
  flow.TakeSyncRequests();
  flow.SyncDispatched(45);
  // A failed sync stops; the next uninstall goes on without syncing.
  action = flow.CloudSettled(45, CloudStatus::Failed);
  CHECK(action.kind == Kind::kStop && action.reason.find("without syncing") != std::string::npos);
  CHECK(flow.Begin(45, false, true) && flow.Step(45) == UninstallStep::kRemoveTitle);
  CHECK(flow.TakeSyncRequests().empty());

  // A running title stops a waiting sync; a game with nothing to sync continues.
  CHECK(flow.Begin(46, false, true));
  CHECK(flow.CloudRefused(46).kind == Kind::kNone);  // not issued yet: nothing to refuse
  flow.TakeSyncRequests();
  CHECK(flow.CloudRefused(46).kind == Kind::kStop && !flow.Step(46));
  CHECK(flow.Begin(47, false, true));
  flow.TakeSyncRequests();
  flow.CloudSkipped(47);
  CHECK(flow.Step(47) == UninstallStep::kRemoveTitle);
  // Unsupported Cloud counts as done.
  CHECK(flow.Begin(48, false, true));
  flow.TakeSyncRequests();
  flow.SyncDispatched(48);
  CHECK(flow.CloudSettled(48, CloudStatus::Unsupported).kind == Kind::kNone && flow.Step(48) == UninstallStep::kRemoveTitle);
  std::puts("uninstall flow tests passed");
  return 0;
}
