// homebrew_live_model_test.cpp - Live homebrew model boundary tests.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#include "homebrew/simulation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

using z3cloudplay::homebrew::AccountUpdate;
using z3cloudplay::homebrew::AuthState;
using z3cloudplay::homebrew::ModelMode;
using z3cloudplay::homebrew::Route;
using z3cloudplay::homebrew::ServiceAction;
using z3cloudplay::homebrew::Simulation;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void activate(Simulation& model, const std::string& key) {
    check(model.action("focus:" + key), "requested live row must exist");
    check(model.action("cross"), "requested live row must activate");
}

bool has_action(const std::vector<ServiceAction>& actions, ServiceAction expected) {
    return std::find(actions.begin(), actions.end(), expected) != actions.end();
}

Simulation make_live(const std::filesystem::path& base) {
    return Simulation(base / "fixtures-that-do-not-exist", base / "missing" / "profile.json", ModelMode::Live);
}

void live_starts_clean_and_never_reads_or_writes_fixtures(const std::filesystem::path& base) {
    auto model = make_live(base);
    const auto& state = model.state();
    check(!state.simulation_mode && state.account.authentication == AuthState::SignedOut,
          "live model starts signed out and identifies itself as live");
    check(state.games.empty() && state.jobs.empty() && state.installations.empty() && state.volumes.empty(),
          "live model does not inherit fixture library or operation state");
    check(state.runtime == z3cloudplay::homebrew::RuntimeState::Unavailable && state.clock_ticks == 0,
          "live model starts without synthetic runtime readiness or fixture time");
    check(!model.view().show_simulation_label && model.snapshot().find("\"simulation\":false") != std::string::npos,
          "live presentation and diagnostics do not claim simulation mode");
    check(model.last_error().empty(), "missing fixture and profile paths are not consulted in live mode");

    const auto clock = state.clock_ticks;
    check(!model.scenario("default"), "scenario API is unavailable in live mode");
    check(!model.action("scenario:qr-approved") && !model.action("inject:qr-approved")
              && !model.action("route:library") && !model.action("install:1:external")
              && !model.action("runtime-retry"),
          "synthetic scenario, injection, route, install and runtime actions are unavailable live");
    model.tick(100);
    check(model.state().clock_ticks == clock && model.state().account.authentication == AuthState::SignedOut,
          "manual ticks cannot advance live authentication or fixture time");
    check(!std::filesystem::exists(base / "missing" / "profile.json"),
          "live interactions do not create the comparison profile");

    model.SetControllerConnected(false);
    check(!model.state().controller_connected, "production controller connection state has an explicit setter");
}

void live_dispatches_bounded_service_effects_and_replaces_challenges(const std::filesystem::path& base) {
    auto model = make_live(base);
    activate(model, "startup-connect");
    auto actions = model.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::BeginQr,
          "connect dispatches one QR begin effect");
    check(model.TakeServiceActions().empty(), "taking service actions drains the queue");

    model.UpdateAccount({AuthState::Waiting, {}, {}, "https://example.invalid/first-secret", {}, false});
    check(model.state().account.challenge_url == "https://example.invalid/first-secret",
          "external challenge is accepted for presentation");
    model.UpdateAccount({AuthState::Waiting, {}, {}, "https://example.invalid/replacement-secret", {}, false});
    check(model.state().account.challenge_url == "https://example.invalid/replacement-secret",
          "new external challenge replaces the prior challenge");
    check(model.snapshot().find("replacement-secret") == std::string::npos,
          "diagnostic snapshot omits the private challenge URL");
    model.UpdateAccount({AuthState::Expired, {}, {}, {}, "Code expired", false});
    check(model.state().account.challenge_url.empty() && model.view().subtitle == "Code expired",
          "external update can clear the challenge and expose a static recovery error");

    activate(model, "qr-refresh");
    model.action("circle");
    actions = model.TakeServiceActions();
    check(actions.size() == 2 && actions[0] == ServiceAction::BeginQr && actions[1] == ServiceAction::CancelQr,
          "retry and cancel dispatch ordered service effects without synthetic approval");

    for (int index = 0; index < 40; ++index) {
        activate(model, "startup-connect");
        model.action("circle");
    }
    actions = model.TakeServiceActions();
    check(actions.size() == 32, "pending service effects are bounded to 32 entries");
}

void failed_signout_preserves_account_until_external_clear(const std::filesystem::path& base) {
    auto model = make_live(base);
    model.UpdateAccount({AuthState::SignedIn, "76561198000000001", "Synthetic Tester", {}, {}, false});
    check(model.state().navigation.route == Route::Home && !model.state().account.cache_available,
          "authentication success does not invent a delivered cache");
    check(model.snapshot().find("76561198000000001") == std::string::npos
              && model.snapshot().find("Synthetic Tester") == std::string::npos,
          "live snapshot omits account identity");

    model.action("l1");
    check(model.state().navigation.route == Route::Settings, "controller navigation reaches account settings");
    activate(model, "settings-signout");
    activate(model, "confirm-destructive");
    auto actions = model.TakeServiceActions();
    check(has_action(actions, ServiceAction::SignOut) && model.state().account.authentication == AuthState::SigningOut,
          "confirmed signout dispatches a barrier and waits for the service result");
    check(model.state().account.id == "76561198000000001",
          "pending signout does not clear account state before durable service completion");

    model.UpdateAccount({AuthState::Failed, "76561198000000001", "Synthetic Tester", {}, "Sign out failed", false});
    check(model.state().account.authentication == AuthState::Failed
              && model.state().account.id == "76561198000000001"
              && model.state().account.error == "Sign out failed"
              && model.view().subtitle == "Sign out failed",
          "failed signout preserves the account and shows the external error without fake success");

    model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, true});
    check(model.state().account.id.empty() && model.state().account.display_name.empty()
              && model.state().games.empty() && model.state().navigation.modals.empty()
              && model.state().navigation.route == Route::Startup,
          "confirmed external account clear removes account-specific state and restores safe startup focus");
}

void account_commands_dispatch_refresh_and_switch_barriers(const std::filesystem::path& base) {
    auto refresh = make_live(base);
    refresh.UpdateAccount({AuthState::SignedIn, "76561198000000002", "Refresh Tester", {}, {}, false});
    refresh.action("l1");
    activate(refresh, "settings-account");
    activate(refresh, "account-sync");
    auto actions = refresh.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::RefreshLibrary
              && refresh.state().account.authentication == AuthState::SignedIn
              && refresh.state().library_loading && refresh.state().library_syncing,
          "account sync dispatches refresh and remains pending external completion");

    auto switching = make_live(base);
    switching.UpdateAccount({AuthState::SignedIn, "76561198000000003", "Switch Tester", {}, {}, false});
    switching.action("l1");
    activate(switching, "settings-account");
    activate(switching, "account-switch");
    activate(switching, "confirm-destructive");
    actions = switching.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::SwitchAccount
              && switching.state().account.authentication == AuthState::SigningOut
              && switching.state().account.id == "76561198000000003",
          "confirmed account switch dispatches a barrier without prematurely clearing identity");
}

void settings_survive_account_restore_and_cache_action_is_honest(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all);
    const auto file = base / "game-settings.json";
    z3cloudplay::runtime::GameSettingsRecord stored;
    stored.revision = 3;
    stored.overrides["resolution"] = "1280x720";
    std::string reason;
    check(z3cloudplay::runtime::SaveGameSettingsFile(file.string(), {{220, stored}}, &reason), "seed live settings");
    auto model = make_live(base);
    model.SetGameSettingsFile(file);
    model.UpdateAccount({AuthState::Restoring, {}, {}, {}, {}, true});
    check(model.state().game_settings.count(220) && model.state().game_settings.at(220) == stored,
          "initial account restore retains device game settings");
    model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, true});
    model.SetWallClock(100);
    std::map<std::uint32_t, z3cloudplay::runtime::GameSettingsRecord> loaded;
    check(z3cloudplay::runtime::LoadGameSettingsFile(file.string(), &loaded, &reason) && loaded.at(220) == stored,
          "sign-out does not erase settings from disk");
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    z3cloudplay::GameSummary game;
    game.id = 220;
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "settings game library delivered");
    {
        std::ofstream corrupt(file);
        corrupt << "malformed settings";
    }
    model.SetGameSettingsFile(file);
    check(model.state().game_settings.count(220) && model.state().game_settings.at(220) == stored,
          "failed settings reload preserves the current records");
    model.action("r1");
    check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "Game settings could not be read",
          "a settings load failure waits in a dialog");
    activate(model, "alert-ok");
    std::ifstream unchanged(file);
    std::string contents;
    std::getline(unchanged, contents);
    check(contents == "malformed settings", "load failure blocks destructive automatic save");
    check(model.action("details:220"), "open settings game details");
    activate(model, "details-more");
    check(std::none_of(model.view().rows.begin(), model.view().rows.end(),
                       [](const auto& row) { return row.key == "game-settings"; }),
          "live settings wait until the game has a prepared PS5 title");
}

const z3cloudplay::homebrew::ViewRow* find_row(const Simulation& model, const std::string& key) {
    const auto& rows = model.view().rows;
    const auto found = std::find_if(rows.begin(), rows.end(), [&key](const auto& row) { return row.key == key; });
    return found == rows.end() ? nullptr : &*found;
}

bool has_notice(const Simulation& model, const std::string& id) {
    const auto& notices = model.state().notices;
    return std::any_of(notices.begin(), notices.end(), [&id](const auto& notice) { return notice.id == id; });
}

// A prepared game's settings and maintenance: rows that need the game closed wait for the
// console's answer, a closed sheet asks for the settings to be applied, and maintenance
// requests go to the runtime after a confirmation.
void live_settings_follow_the_prepared_title(const std::filesystem::path& base) {
    using z3cloudplay::homebrew::MaintenanceKind;
    using z3cloudplay::homebrew::TitleActivity;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "prepared-settings.json");
    model.SetSettingsData({{"z3cloudplay-default", "Z3CloudPlay default", "Most games", "", {}},
                           {"mouse", "Mouse-driven game", "Point and click", "", {{"controller", "keyboard"}}}}, {});
    model.SetKitCapabilities(z3cloudplay::runtime::KitCapabilitiesFromManifest({"settings-dxvk-config"}, true));
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    z3cloudplay::GameSummary game;
    game.id = 220;
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "prepared game library delivered");
    model.UpdateInstalls({}, {{220, 1024, "internal", true}});
    z3cloudplay::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = 220;
    runtime.status = "Ready";
    runtime.ready = runtime.prepared = runtime.has_title = true;
    runtime.launch_entry_id = "game";
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:220"), "open prepared game details");
    activate(model, "details-more");
    check(find_row(model, "game-settings") && find_row(model, "game-maintenance"),
          "a prepared game's More menu offers Game settings and Maintenance");

    activate(model, "game-settings");
    check(model.TakeActivityRequests() == std::vector<z3cloudplay::homebrew::GameId>{220}, "the sheet asks whether the game runs");
    activate(model, "section-more");
    const auto* version = find_row(model, "setting-windows_version");
    check(version && !version->enabled && version->subtitle == "Checking whether Half-Life 2 is running…",
          "game-closed rows wait for the console's answer");
    runtime.activity = TitleActivity::Running;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(find_row(model, "setting-windows_version")->subtitle == "Quit Half-Life 2 to change this.",
          "game-closed rows are disabled while the game runs");
    runtime.activity = TitleActivity::Stopped;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(find_row(model, "setting-windows_version")->enabled, "game-closed rows open once the game is stopped");
    check(find_row(model, "setting-locale") && !find_row(model, "setting-locale")->enabled,
          "unbuilt rows stay listed and disabled");
    check(model.action("focus:setting-frame_limit") &&
              find_row(model, "setting-frame_limit")->subtitle.find("Also called dxgi.maxFrameRate") != std::string::npos,
          "the focused row names its technical keys");

    activate(model, "setting-resolution");
    activate(model, "choice-0");
    check(model.action("circle"), "close the settings sheet");
    check(model.TakeMaintenanceRequests() ==
              std::vector<std::pair<z3cloudplay::homebrew::GameId, MaintenanceKind>>{{220, MaintenanceKind::ApplySettings}},
          "closing the sheet asks for the settings to be applied");
    check(has_notice(model, "settings-saved"), "closing the sheet announces the change");
    activate(model, "details-more");
    check(find_row(model, "game-settings")->subtitle == "Changes apply on next launch",
          "the More menu says unpublished changes wait for the next launch");

    activate(model, "game-maintenance");
    check(model.TakeActivityRequests() == std::vector<z3cloudplay::homebrew::GameId>{220}, "maintenance asks whether the game runs");
    check(!find_row(model, "maintenance-backup")->enabled, "maintenance waits for the console's answer");
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(find_row(model, "maintenance-backup")->enabled, "maintenance is open while the game is stopped");
    activate(model, "maintenance-backup");
    activate(model, "confirm-destructive");
    check(model.TakeMaintenanceRequests() ==
              std::vector<std::pair<z3cloudplay::homebrew::GameId, MaintenanceKind>>{{220, MaintenanceKind::BackupSaves}},
          "a confirmed maintenance action goes to the runtime");
    check(find_row(model, "maintenance-backup")->value == "Working…", "the running action shows its progress");
    runtime.maintenance = MaintenanceKind::BackupSaves;
    runtime.maintenance_running = true;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    runtime.maintenance_running = false;
    runtime.maintenance_ok = true;
    runtime.maintenance_message = "Saves backed up.";
    runtime.save_backups = 1;
    runtime.latest_save_backup = 1760000000;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(has_notice(model, "maintenance-220"), "a finished maintenance action reports its outcome");
    check(find_row(model, "maintenance-backup")->value.rfind("1 backup", 0) == 0, "the backup row counts the backups");
    runtime.activity = TitleActivity::Running;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(!find_row(model, "maintenance-shader")->enabled, "maintenance waits while the game runs");
}


// A new program applies like any other setting: closing Game settings asks the runtime to apply
// it (RuntimeHost::RepublishSettings switches the prepared title's program), with no dialog, and
// Play starts at once. Reset to recipe goes back to Steam's launch option.
void live_program_change_applies_like_other_settings(const std::filesystem::path& base) {
    using z3cloudplay::homebrew::GameId;
    using z3cloudplay::homebrew::MaintenanceKind;
    using z3cloudplay::homebrew::TitleActivity;
    using Requests = std::vector<std::pair<GameId, MaintenanceKind>>;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "program-settings.json");
    model.SetSettingsData({{"z3cloudplay-default", "Z3CloudPlay default", "Most games", "", {}}}, {});
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    z3cloudplay::GameSummary game;
    game.id = 22380;
    game.title = "Fallout: New Vegas";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "program: library delivered");
    model.UpdateInstalls({}, {{22380, 1024, "internal", true}});
    // Prepared for a program picked earlier, so the recipe lists it first, as on the console.
    z3cloudplay::runtime::GameRecipe recipe;
    recipe.id = z3cloudplay::runtime::GeneratedRecipeId(22380);
    recipe.version = 2;
    recipe.app_id = 22380;
    recipe.name = game.title;
    recipe.launch_entries = {{"program", "FalloutNV.exe", "FalloutNV.exe", "", ""},
                             {"launch-1", "Fallout: New Vegas", "FalloutNVLauncher.exe", "", ""}};
    model.SetGeneratedRecipes({recipe});
    std::map<GameId, z3cloudplay::homebrew::LiveGameSetup> setups;
    setups[22380].scanned = true;
    setups[22380].programs = {{"FalloutNV.exe", "pe32", "", false, ""},
                              {"FalloutNVLauncher.exe", "pe32", "", false, ""},
                              {"Redists/vcredist_x86.exe", "pe32", "", false, ""}};
    model.SetGameSetups(setups);
    z3cloudplay::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = 22380;
    runtime.status = "Ready";
    runtime.ready = runtime.prepared = runtime.has_title = runtime.can_play = true;
    runtime.launch_entry_id = "launch-1";
    runtime.activity = TitleActivity::Stopped;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:22380"), "program: open details");

    // The Program picker lists each program once: Steam's launcher as its entry, the earlier pick as
    // the folder program it is, and the subtitle names the default by its label.
    activate(model, "details-more");
    activate(model, "game-settings");
    (void)model.TakeActivityRequests();
    activate(model, "setting-launch_option");
    check(!find_row(model, "choice-0") && find_row(model, "choice-1") && find_row(model, "program-0") &&
              !find_row(model, "program-1") && find_row(model, "program-2"),
          "picker: no program is listed twice");
    check(model.view().subtitle.find("Recreate") == std::string::npos, "program: no Recreate bottle is needed");
    activate(model, "program-0");
    check(find_row(model, "setting-launch_option")->badge.empty() &&
              find_row(model, "setting-launch_option")->value == "FalloutNV.exe",
          "program: the Program row is an ordinary setting");
    check(model.action("circle") && model.state().navigation.modals.empty(), "program: closing the sheet asks nothing");
    check(model.TakeMaintenanceRequests() == Requests{{22380, MaintenanceKind::ApplySettings}},
          "program: closing the sheet applies the new program");
    activate(model, "details-play");
    check(model.state().navigation.modals.empty() && model.TakeRuntimePlayRequests() == std::vector<GameId>{22380},
          "program: Play starts at once");

    // Reset to recipe goes back to Steam's launch option, not the program the recipe lists first.
    activate(model, "details-more");
    activate(model, "game-settings");
    activate(model, "setting-launch_option");
    check(model.view().subtitle.find("Your choice: FalloutNV.exe. Applies on next launch.") != std::string::npos,
          "picker: the choice is named by its program, not its id, and applies on next launch");
    activate(model, "value-reset");
    check(find_row(model, "setting-launch_option")->value == "Fallout: New Vegas", "program: reset goes back to Steam's");
    check(model.action("circle") && model.state().navigation.modals.empty() &&
              model.TakeMaintenanceRequests() == Requests{{22380, MaintenanceKind::ApplySettings}},
          "program: the reset applies the same way");
}

void live_download_telemetry() {
    using namespace z3cloudplay::homebrew;
    Simulation model({}, {}, ModelMode::Live);
    LiveInstallJob job; job.game_id = 275390; job.state = JobState::Downloading;
    job.done_bytes = 100000000; job.total_bytes = 1000000000;
    job.bytes_per_second = 40055603; job.eta_seconds = 300;
    model.UpdateInstalls({job}, {});
    check(model.state().jobs.size() == 1 && model.state().jobs[0].bytes_per_second == 40055603 &&
          model.state().jobs[0].eta_seconds == 300, "live transfer telemetry reaches the Downloads model");
    check(FormatDownloadRate(job.bytes_per_second, job.eta_seconds) == "38.2 MB/s · ~5 min left", "rate and ETA formatting");
    check(FormatDownloadRate(0, {}) == "0.0 MB/s · —", "stalled rate has no invented ETA");
    check(FormatDownloadRate(1048576, {}) == "1.0 MB/s · —", "unknown total keeps rate but hides ETA");
    check(FormatDownloadRate({}, 300) == "—", "unknown rate hides stale ETA");
    check(FormatDownloadRate(1048576, 59) == "1.0 MB/s · ~59 sec left", "short ETA");
    check(FormatDownloadRate(1048576, 61) == "1.0 MB/s · ~2 min left", "ETA rounds up");
    for (const auto state : {JobState::Paused, JobState::Queued, JobState::Failed}) {
        job.state = state; // stale incoming fields must never appear outside an active transfer
        model.UpdateInstalls({job}, {});
        check(!model.state().jobs[0].bytes_per_second && !model.state().jobs[0].eta_seconds, "inactive telemetry clears");
    }
}

} // namespace

int main() {
    live_download_telemetry();
    const auto base = std::filesystem::canonical(std::filesystem::temp_directory_path())
        / ("z3cloudplay-homebrew-live-model-test-" + std::to_string(getpid()));
    std::error_code error;
    std::filesystem::remove_all(base, error);

    settings_survive_account_restore_and_cache_action_is_honest(base);
    live_settings_follow_the_prepared_title(base);
    live_program_change_applies_like_other_settings(base);
    live_starts_clean_and_never_reads_or_writes_fixtures(base);
    live_dispatches_bounded_service_effects_and_replaces_challenges(base);
    failed_signout_preserves_account_until_external_clear(base);
    account_commands_dispatch_refresh_and_switch_barriers(base);

    std::filesystem::remove_all(base, error);
    std::cout << "homebrew_live_model_test passed\n";
    return 0;
}
