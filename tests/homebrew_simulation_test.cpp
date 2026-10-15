// homebrew_simulation_test.cpp - Deterministic Z3CloudPlay UI simulation acceptance tests.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#include "homebrew/simulation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_profile() {
    auto path = std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("z3cloudplay-homebrew-simulation-test-" + std::to_string(getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

void t01_authentication_ignores_late_approval(const std::filesystem::path& fixtures,
                                               const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("startup-loading");
    check(simulation.view().rows.front().key == "startup-loading", "T01 startup exposes deterministic loading");
    simulation.tick(1);
    check(simulation.view().rows.front().key == "startup-connect", "T01 advancing the clock completes startup loading");
    simulation.scenario("signed-out");
    check(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Startup,
          "T01 signed-out scenario starts at first run");
    simulation.action("cross");
    check(simulation.state().account.authentication == z3cloudplay::homebrew::AuthState::Waiting,
          "T01 Connect Steam opens a waiting challenge");
    z3cloudplay::homebrew::Simulation waiting_restart(fixtures, profile);
    check(waiting_restart.state().account.authentication == z3cloudplay::homebrew::AuthState::SignedOut
              && waiting_restart.state().navigation.route == z3cloudplay::homebrew::Route::Startup
              && waiting_restart.state().account.id.empty() && waiting_restart.state().account.display_name.empty()
              && !waiting_restart.state().account.cache_available,
          "T01 restarting an anonymous waiting challenge invalidates it without leaking catalog identity");
    simulation.scenario("qr-expired");
    check(simulation.state().account.authentication == z3cloudplay::homebrew::AuthState::Expired,
          "T01 injected QR expiry is visible");
    simulation.action("cross");
    simulation.scenario("qr-approved");
    simulation.tick(1);
    check(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Home,
          "T01 approved refreshed challenge reaches Home");
    simulation.scenario("signed-out");
    simulation.action("cross");
    simulation.action("circle");
    simulation.scenario("late-approval");
    simulation.tick(5);
    check(simulation.state().account.authentication == z3cloudplay::homebrew::AuthState::Cancelled,
          "T01 late approval cannot revive a cancelled challenge");
    simulation.scenario("signed-out"); simulation.action("focus:startup-skip"); simulation.action("cross");
    check(std::any_of(simulation.view().rows.begin(), simulation.view().rows.end(),
                      [](const z3cloudplay::homebrew::ViewRow& row) { return row.key == "home-connect"; }),
          "T01 empty signed-out Home offers both Library and Connect");
    simulation.scenario("signed-out"); simulation.action("cross"); simulation.action("inject:qr-approved");
    z3cloudplay::homebrew::Simulation syncing_restart(fixtures, profile);
    check(syncing_restart.state().account.authentication == z3cloudplay::homebrew::AuthState::Failed
              && syncing_restart.state().navigation.route == z3cloudplay::homebrew::Route::SignIn
              && syncing_restart.state().navigation.focus_key == "qr-retry",
          "T01 interrupted sync restarts at a controller-recoverable retry state");
    syncing_restart.action("cross");
    check(syncing_restart.state().account.authentication == z3cloudplay::homebrew::AuthState::Waiting,
          "T01 retry creates a fresh challenge after interrupted sync");
}

void t02_focus_and_nested_modal_restoration(const std::filesystem::path& fixtures,
                                             const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("default");
    simulation.action("r1");
    simulation.action("down");
    simulation.action("down");
    const auto selected = simulation.state().navigation.selected_game_id;
    const auto scroll = simulation.state().navigation.scroll_offsets.at("library-grid");
    simulation.action("cross");
    simulation.action("options");
    check(simulation.state().navigation.modals.size() == 1, "T02 More opens one modal");
    simulation.action("circle");
    check(simulation.state().navigation.focus_key == "details-more", "T02 closing More restores its opener");
    simulation.action("circle");
    check(simulation.state().navigation.selected_game_id == selected,
          "T02 returning to Library restores selected game identity");
    check(simulation.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "T02 returning to Library restores exact viewport");
}

void public_contract_and_snapshot(const std::filesystem::path& fixtures,
                                  const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("default");
    check(simulation.state().games.size() == 24, "curated fixture exposes all 24 games");
    check(!simulation.view().rows.empty(), "active surface exposes reusable rows");
    check(simulation.snapshot().find("\"simulation\":true") != std::string::npos,
          "snapshot marks the state as a UI simulation");
}

void activate(z3cloudplay::homebrew::Simulation& simulation, const std::string& key) {
    check(simulation.action("focus:" + key), "test focus key exists");
    check(simulation.action("cross"), "focused action activates");
}

void t03_circle_leaves_an_applied_search(const std::filesystem::path& fixtures,
                                         const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:library");
    const auto has_legend = [&](const std::string& legend) {
        const auto& legends = simulation.view().legends;
        return std::find(legends.begin(), legends.end(), legend) != legends.end();
    };
    check(has_legend("Touchpad Search") && !has_legend("Search") && !has_legend("Circle Clear search"),
          "T03 Library names the touchpad for Search");
    simulation.action("search");
    check(has_legend("Options Done"), "T03 the keyboard names Options for Done");
    simulation.text("Night Orchard");
    simulation.action("options");
    check(!simulation.state().editor.active && simulation.state().library.query == "Night Orchard",
          "T03 Options commits the keyboard like Done");
    check(simulation.visible_games().size() == 1 && has_legend("Circle Clear search"),
          "T03 an applied search offers Circle to clear it");
    simulation.action("circle");
    check(simulation.state().library.query.empty() && simulation.visible_games().size() > 1
              && simulation.state().navigation.route == z3cloudplay::homebrew::Route::Library,
          "T03 Circle clears the search and stays in the Library");
    simulation.action("circle");
    check(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Home,
          "T03 Circle without a search goes back Home");
}

void t03_search_filters_and_focus_reconciliation(const std::filesystem::path& fixtures,
                                                 const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:library");
    simulation.action("search");
    simulation.text("temporary");
    check(simulation.visible_games().empty() && simulation.view().subtitle.find("0 results") != std::string::npos,
          "T03 search result count updates live before commit");
    simulation.action("circle");
    check(simulation.state().library.query.empty(), "T03 cancelling search restores the original query");
    simulation.action("search");
    simulation.text("Night Orchard");
    simulation.action("done");
    check(simulation.visible_games().size() == 1 && simulation.visible_games().front()->id == 900010,
          "T03 committed search updates results and count");
    check(simulation.state().navigation.selected_game_id == 900010 && simulation.state().navigation.focus_key == "game-900010",
          "T03 committing search reconciles logical selection to the visible result");
    const bool prior_favorite = simulation.state().library.favorites.contains(900001);
    simulation.action("square");
    check(simulation.state().library.favorites.contains(900010) && simulation.state().library.favorites.contains(900001) == prior_favorite,
          "T03 Square after controller search favorites the focused result");
    simulation.action("square");
    activate(simulation, "game-900010");
    simulation.action("circle");
    simulation.action("options");
    activate(simulation, "hide");
    check(simulation.visible_games().empty() && simulation.state().navigation.focus_key == "clear-search",
          "T03 removing the focused result reconciles focus to Clear search");
    activate(simulation, "clear-search");
    check(!simulation.visible_games().empty(), "T03 Clear search recovers results");
    simulation.action("triangle");
    activate(simulation, "sort-playtime");
    simulation.action("triangle");
    activate(simulation, "filter-controller-full");
    simulation.action("triangle");
    activate(simulation, "filters-reset");
    check(simulation.state().library.filters.empty() && simulation.state().library.sort == "name",
          "T03 reset clears every filter and restores name sort");
    simulation.action("select:900012");
    const auto selected = simulation.state().navigation.selected_game_id;
    const auto scroll = simulation.state().navigation.scroll_offsets.at("library-grid");
    simulation.action("inject:library-refresh");
    check(simulation.state().library_loading && simulation.view().subtitle.find("Refreshing") != std::string::npos,
          "T03 refresh has a visible loading state before completion");
    simulation.tick(1);
    check(!simulation.state().library_loading && simulation.state().navigation.selected_game_id == selected
              && simulation.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "T03 refresh preserves exact library selection and viewport");
}

void t04_organization_persists_without_deleting_games(const std::filesystem::path& fixtures,
                                                       const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:library");
    simulation.action("select:900012");
    simulation.action("square");
    check(simulation.state().library.favorites.contains(900012), "T04 favorite toggles by stable ID");
    check(simulation.action("collection:create:Night games"), "T04 collection create action is available");
    check(simulation.action("collection:add:collection-2:900012"), "T04 collection membership changes");
    check(simulation.action("collection:rename:collection-2:After dark"), "T04 collection rename works");
    check(simulation.action("collection:pin:collection-2"), "T04 collection pin works");
    check(simulation.action("rate:900012:playable"), "T04 local compatibility rating works");
    check(simulation.action("note:900012:Synthetic note"), "T04 note edit works");
    z3cloudplay::homebrew::Simulation organized(fixtures, profile);
    check(organized.state().library.collections.size() == 2 && organized.state().library.collections.back().pinned
              && organized.state().library.collections.back().members.contains(900012),
          "T04 collection name, pin, and membership survive restart");
    check(organized.state().library.ratings.at(900012) == z3cloudplay::homebrew::Compatibility::Playable
              && organized.state().library.notes.at(900012) == "Synthetic note",
          "T04 rating and note survive restart");
    check(simulation.action("collection:delete:collection-2"), "T04 collection delete works");
    check(simulation.state().games.size() == 24, "T04 deleting a collection never deletes a game");
    z3cloudplay::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().library.favorites.contains(900012), "T04 favorites survive restart");
}

void t05_install_rechecks_storage_and_submits_once(const std::filesystem::path& fixtures,
                                                   const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("details:900012");
    activate(simulation, "details-install");
    activate(simulation, "storage-picker");
    activate(simulation, "volume-external");
    simulation.action("inject:storage-disconnect");
    activate(simulation, "install-confirm");
    check(simulation.state().jobs.size() == 3, "T05 disconnect before confirmation blocks install");
    simulation.action("inject:storage-reconnect");
    activate(simulation, "install-confirm");
    check(simulation.state().jobs.size() == 4, "T05 successful confirmation creates one job");
    check(simulation.action("install:900012:external"), "T05 duplicate install submission is accepted idempotently");
    check(simulation.state().jobs.size() == 4, "T05 duplicate confirmation cannot create a second job");
}

void t06_queue_transitions_are_single_active_and_safe(const std::filesystem::path& fixtures,
                                                      const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:downloads");
    activate(simulation, "job-job-1");
    activate(simulation, "job-pause-job-1");
    simulation.tick(1);
    activate(simulation, "job-job-1");
    activate(simulation, "job-resume-job-1");
    int active = 0;
    for (const auto& job : simulation.state().jobs)
        if (job.state == z3cloudplay::homebrew::JobState::Downloading || job.state == z3cloudplay::homebrew::JobState::Verifying) ++active;
    check(active == 1, "T06 resume preserves the one-active-transfer invariant");
    check(simulation.action("job:move-up:job-3"), "T06 queued jobs can move up");
    check(simulation.action("job:cancel:job-2"), "T06 cancel opens safe confirmation");
    check(simulation.state().navigation.modals.back().focused_action == 0, "T06 destructive cancel defaults to decline");
    simulation.action("circle");
    check(simulation.state().jobs[1].state != z3cloudplay::homebrew::JobState::Cancelled, "T06 declining cancel leaves the job intact");
    simulation.action("job:fail:job-1:download");
    check(simulation.action("job:retry:job-1"), "T06 failure retry is reachable");
    check(simulation.state().jobs.front().state == z3cloudplay::homebrew::JobState::Downloading,
          "T06 retry resumes the failed stage");
    simulation.action("details:900001"); simulation.action("options"); activate(simulation, "verify");
    simulation.action("details:900001"); simulation.action("options"); activate(simulation, "verify");
    const auto verifies = std::count_if(simulation.state().jobs.begin(), simulation.state().jobs.end(),
        [](const z3cloudplay::homebrew::Job& job) { return job.kind == "verify" && job.game_id == 900001; });
    check(verifies == 1, "T06 repeated Verify creates one queued operation");
    active = 0;
    for (const auto& job : simulation.state().jobs)
        if (job.state == z3cloudplay::homebrew::JobState::Downloading || job.state == z3cloudplay::homebrew::JobState::Verifying) ++active;
    check(active == 1, "T06 queued verification preserves one active transfer");
}

void t07_interruption_and_verification_recovery(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("inject:storage-disconnect");
    z3cloudplay::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().jobs.front().state == z3cloudplay::homebrew::JobState::Interrupted,
          "T07 interrupted work survives restart without invented completion");
    restored.action("inject:storage-reconnect");
    check(restored.action("job:retry:job-1"), "T07 interrupted download can recover");
    restored.action("inject:verification-failed");
    check(restored.state().jobs.front().retry_stage == z3cloudplay::homebrew::JobStage::Verify,
          "T07 verification failure preserves its retry stage");
    restored.scenario("runtime-unavailable");
    check(!restored.state().installations.front().runtime_ready,
          "T07 downloaded/installed remains distinct from runtime readiness");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Bundled presets and the HL2 recipe, re-targeted at fixture game 900001.
void load_settings_data(z3cloudplay::homebrew::Simulation& simulation, const std::filesystem::path& fixtures) {
    namespace rt = z3cloudplay::runtime;
    const auto source = fixtures.parent_path().parent_path();
    std::vector<rt::SettingsPreset> presets;
    rt::GameRecipe recipe;
    std::string reason;
    check(rt::ParseSettingsPresets(read_text(source / "runtime/presets.json"), &presets, &reason), "presets load");
    check(rt::ParseGameRecipe(read_text(source / "runtime/recipes/steam-220-hl2-v1.json"), &recipe, &reason), "recipe loads");
    recipe.app_id = 900001;
    recipe.launch_entries.push_back({"hl2-lostcoast", "Lost Coast", "hl2.exe", ""});
    simulation.SetSettingsData(std::move(presets), {std::move(recipe)});
}

const z3cloudplay::homebrew::ViewRow* find_row(const z3cloudplay::homebrew::Simulation& simulation, const std::string& key) {
    for (const auto& row : simulation.view().rows)
        if (row.key == key) return &row;
    return nullptr;
}

void t08_nested_settings_restore_openers(const std::filesystem::path& fixtures,
                                         const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    check(find_row(simulation, "setting-resolution") && find_row(simulation, "setting-resolution")->value == "1920×1080",
          "T08 sheet shows the recipe resolution");
    check(find_row(simulation, "setting-frame_limit") && !find_row(simulation, "setting-frame_limit")->enabled,
          "T08 dxvk.conf rows are listed but disabled until the kit reads dxvk.conf");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-2");
    check(simulation.state().navigation.focus_key == "setting-resolution", "T08 picker restores its opener");
    check(simulation.state().game_settings.at(900001).overrides.at("resolution") == "2560x1440", "T08 picker saves the override");
    check(find_row(simulation, "setting-resolution")->modified, "T08 changed row is marked");
    check(find_row(simulation, "preset-picker")->value == "Custom · from Z3CloudPlay default", "T08 preset reads Custom after a change");
    activate(simulation, "setting-extra_arguments");
    simulation.text("--fixture");
    simulation.action("circle");
    check(!simulation.state().game_settings.at(900001).overrides.count("extra_arguments"), "T08 cancelling text edit discards changes");
    activate(simulation, "setting-extra_arguments");
    simulation.text("--fixture");
    simulation.action("done");
    check(simulation.state().game_settings.at(900001).overrides.at("extra_arguments") == "--fixture",
          "T08 committing text edit saves the override");
    activate(simulation, "setting-extra_arguments");
    simulation.text(" \"unclosed");
    simulation.action("done");
    check(simulation.state().editor.active && simulation.state().game_settings.at(900001).overrides.at("extra_arguments") == "--fixture",
          "T08 invalid arguments keep the editor open and the old value");
    simulation.action("circle");
    // Choosing the inherited value removes the override instead of storing a copy.
    activate(simulation, "setting-controller");
    activate(simulation, "choice-0");
    check(!simulation.state().game_settings.at(900001).overrides.count("controller"), "T08 inherited choice is not an override");
    // Triangle resets the focused row to the preset value.
    check(simulation.action("focus:setting-resolution") && simulation.action("triangle"), "T08 triangle resets");
    check(!simulation.state().game_settings.at(900001).overrides.count("resolution"), "T08 reset removes the override");
    simulation.action("circle");
    simulation.action("details:900002");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "setting-extra_arguments");
    check(simulation.state().editor.value.empty(), "T08 custom arguments do not leak to another game");
    simulation.action("circle");
    simulation.action("circle");
    z3cloudplay::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().game_settings.at(900001).overrides.at("extra_arguments") == "--fixture"
              && !restored.state().game_settings.count(900002),
          "T08 per-game overrides persist independently");
}

void t08b_settings_sheet_rules(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = z3cloudplay::runtime;
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    // More settings expands inline; graphics is locked by the recipe and scaling is
    // disabled on DXVK with its reason.
    check(!find_row(simulation, "setting-graphics"), "T08b More settings starts collapsed");
    activate(simulation, "section-more");
    const auto* graphics = find_row(simulation, "setting-graphics");
    check(graphics && graphics->enabled && graphics->badge == "Required by this game", "T08b graphics is locked by the recipe");
    const auto* scaling = find_row(simulation, "setting-scaling");
    check(scaling && !scaling->enabled && scaling->subtitle == "Not available with DXVK output yet.", "T08b scaling shows its reason");
    activate(simulation, "setting-graphics");
    check(simulation.state().navigation.modals.back().title == "Change anyway?", "T08b a locked row asks before it changes");
    simulation.action("circle");
    check(simulation.state().navigation.modals.back().id == "game-settings", "T08b locked rows do not open a picker");
    // Launch options come from the recipe.
    activate(simulation, "setting-launch_option");
    check(find_row(simulation, "choice-1") && find_row(simulation, "choice-1")->title == "Lost Coast", "T08b launch options list recipe entries");
    activate(simulation, "choice-1");
    // A preset clears overrides and becomes the base.
    activate(simulation, "preset-picker");
    activate(simulation, "preset-mouse-driven");
    const auto& record = simulation.state().game_settings.at(900001);
    check(record.preset_id == "mouse-driven" && record.overrides.empty(), "T08b choosing a preset clears overrides");
    check(find_row(simulation, "preset-picker")->value == "Mouse-driven game", "T08b preset row names the preset");
    rt::EffectiveSettings settings;
    check(simulation.EffectiveSettingsFor(900001, &settings) && settings.values.at("controller").value == "keyboard",
          "T08b preset values apply");
    // The sheet's settings produce a profile prospero-win accepts.
    rt::ProsperoProfile generated;
    std::string reason;
    check(rt::RenderProsperoProfile(simulation.RecipeFor(900001), settings, {"z3cloudplay-steam-900001", "/data/z3cloudplay/games/900001"},
                                    &generated, &reason), "T08b sheet settings render a valid prospero profile");
    // Reset all returns to the preset.
    activate(simulation, "setting-pointer_speed");
    activate(simulation, "choice-4");
    check(!simulation.state().game_settings.at(900001).overrides.empty(), "T08b override recorded");
    activate(simulation, "reset-settings");
    check(simulation.state().game_settings.at(900001).overrides.empty(), "T08b reset all clears overrides");
    simulation.action("circle");
    const auto toast = std::find_if(simulation.state().notices.begin(), simulation.state().notices.end(),
                                    [](const z3cloudplay::homebrew::Notice& notice) { return notice.id == "settings-saved"; });
    check(toast != simulation.state().notices.end() && toast->message == "Settings saved · Mouse-driven game"
              && toast->detail == "1 change applies on next launch",
          "T08b closing the sheet announces what changed");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    // dxvk.conf rows appear when the kit reads dxvk.conf.
    rt::KitCapabilities kit;
    kit.dxvk_config = true;
    simulation.SetKitCapabilities(kit);
    check(find_row(simulation, "setting-frame_limit") && find_row(simulation, "setting-frame_limit")->enabled,
          "T08b frame limit appears once the kit supports it");
    // Button mapping: inherit shows the recipe binding; a common key and a typed key save.
    activate(simulation, "section-more");
    activate(simulation, "button-mapping");
    check(find_row(simulation, "setting-button.options")->value == "Escape", "T08b recipe binding shows");
    activate(simulation, "setting-button.touchpad");
    activate(simulation, "choice-17");
    check(simulation.state().game_settings.at(900001).overrides.at("button.touchpad") == "f5", "T08b common key saves");
    activate(simulation, "setting-button.l3");
    activate(simulation, "choice-custom");
    simulation.text("vk:0x41");
    simulation.action("done");
    check(simulation.state().game_settings.at(900001).overrides.at("button.l3") == "vk:0x41", "T08b typed key saves");
    activate(simulation, "setting-button.touchpad");
    activate(simulation, "choice-0");
    check(!simulation.state().game_settings.at(900001).overrides.count("button.touchpad"), "T08b inherit removes the binding");
    simulation.action("circle");
    check(find_row(simulation, "button-mapping")->modified, "T08b changed bindings mark the row");
    check(!find_row(simulation, "clear-shader-cache"), "T08b maintenance is not in the settings sheet");
    // Maintenance lives on the game page, each action behind a confirmation.
    simulation.action("circle");
    activate(simulation, "details-more");
    activate(simulation, "game-maintenance");
    check(find_row(simulation, "maintenance-shader") && find_row(simulation, "maintenance-shader")->badge == "Game must be closed",
          "T08b maintenance lists clear shader cache");
    check(find_row(simulation, "maintenance-recreate")->badge == "Prepares bottle", "T08b recreate prepares the bottle");
    simulation.action("r1");
    check(simulation.state().navigation.modals.back().focused_action == 5, "T08b R1 jumps five rows in maintenance");
    simulation.action("l1");
    check(simulation.view().legends.size() == 3 && simulation.view().legends[1] == "L1/R1 Jump", "T08b maintenance names the jump");
    activate(simulation, "maintenance-shader");
    check(simulation.state().navigation.modals.back().id == "confirmation", "T08b maintenance asks first");
    activate(simulation, "confirm-destructive");
    check(simulation.state().navigation.modals.back().id == "game-maintenance", "T08b confirming returns to maintenance");
    check(std::any_of(simulation.state().notices.begin(), simulation.state().notices.end(),
                      [](const z3cloudplay::homebrew::Notice& notice) { return notice.id == "maintenance-900001"; }),
          "T08b clear shader cache reports");
    // Games without a recipe still get a usable sheet.
    simulation.action("circle");
    simulation.action("details:900003");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    check(find_row(simulation, "setting-resolution") && find_row(simulation, "setting-resolution")->value == "1920×1080",
          "T08b games without a recipe show Z3CloudPlay defaults");
}

// The preset chooser compares the focused preset with the current values; the Community
// entry is listed but waits for the shared index; L1/R1 jump five rows; Advanced ends with
// the read-only effective command.
void settings_sheet_details(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "preset-picker");
    check(simulation.view().comparison.has_value(), "preset chooser compares the focused preset");
    check(simulation.action("down"), "focus the next preset");
    const auto& comparison = *simulation.view().comparison;
    check(!comparison.rows.empty() && !comparison.note.empty() && !comparison.title.empty(),
          "the comparison lists settings and closes with what applies");
    const auto* community = find_row(simulation, "preset-community");
    check(community && !community->enabled && community->value == "Not available yet", "community presets wait for the index");
    simulation.action("focus:preset-community");
    simulation.action("cross");
    check(simulation.state().navigation.modals.back().id == "preset-picker", "the community entry does nothing yet");
    simulation.action("circle");
    simulation.action("focus:preset-picker");
    simulation.action("r1");
    check(simulation.state().navigation.modals.back().focused_action == 5, "R1 jumps five rows");
    simulation.action("l1");
    check(simulation.state().navigation.modals.back().focused_action == 0, "L1 jumps back");
    activate(simulation, "section-advanced");
    const auto* command = find_row(simulation, "effective-command");
    check(command && command->value == "Read only" && !command->subtitle.empty(), "Advanced ends with the effective command");
    activate(simulation, "effective-command");
    check(simulation.state().navigation.modals.back().title == "Effective command" &&
              !simulation.state().navigation.modals.back().body.empty(),
          "the effective command opens in full");
    simulation.action("circle");
    simulation.action("circle");
}

void settings_revision_exhaustion_preserves_record(const std::filesystem::path& fixtures,
                                                 const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    const auto settings_file = profile.string() + ".settings";
    z3cloudplay::runtime::GameSettingsRecord record;
    record.revision = std::numeric_limits<std::uint64_t>::max();
    record.overrides["resolution"] = "1280x720";
    std::string reason;
    check(z3cloudplay::runtime::SaveGameSettingsFile(settings_file, {{900001, record}}, &reason), "seed exhausted revision");
    simulation.SetGameSettingsFile(settings_file);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-2");
    check(simulation.state().game_settings.at(900001) == record,
          "exhausted settings revision rejects change without wrapping");
    simulation.action("circle");
    z3cloudplay::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().game_settings.count(900001) && restored.state().game_settings.at(900001) == record,
          "fixture settings restore preserves the exact uint64 revision");
    record.revision = 9007199254740993ULL;
    check(z3cloudplay::runtime::SaveGameSettingsFile(settings_file, {{900001, record}}, &reason), "seed exact revision above double precision");
    simulation.SetGameSettingsFile(settings_file);
    simulation.action("circle");
    z3cloudplay::homebrew::Simulation precise(fixtures, profile);
    check(precise.state().game_settings.count(900001) && precise.state().game_settings.at(900001) == record,
          "fixture revision cannot round through double precision");
    std::filesystem::remove(settings_file);
}

void settings_save_failure_does_not_claim_success(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    std::filesystem::remove(profile);
    std::filesystem::create_directory(profile);
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-2");
    simulation.action("circle");
    check(std::none_of(simulation.state().notices.begin(), simulation.state().notices.end(),
                       [](const auto& notice) { return notice.id == "settings-saved"; }),
          "failed settings persistence cannot announce saved");
    std::filesystem::remove_all(profile);
}

void t09_uninstall_and_reset_preserve_declared_data(const std::filesystem::path& fixtures,
                                                    const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("details:900001");
    simulation.action("options");
    activate(simulation, "uninstall");
    check(simulation.state().navigation.modals.back().focused_action == 0, "T09 uninstall defaults to Cancel");
    simulation.action("circle");
    simulation.action("circle");
    simulation.action("options");
    activate(simulation, "uninstall");
    activate(simulation, "confirm-destructive");
    simulation.action("details:900001"); simulation.action("options"); activate(simulation, "uninstall"); activate(simulation, "confirm-destructive");
    const auto uninstalls = std::count_if(simulation.state().jobs.begin(), simulation.state().jobs.end(),
        [](const z3cloudplay::homebrew::Job& job) { return job.game_id == 900001 && job.kind == "uninstall" && job.state != z3cloudplay::homebrew::JobState::Cancelled; });
    check(uninstalls == 1, "T09 repeated uninstall confirmation is idempotent");
    int active = 0;
    for (const auto& job : simulation.state().jobs)
        if (job.state == z3cloudplay::homebrew::JobState::Downloading || job.state == z3cloudplay::homebrew::JobState::Verifying) ++active;
    check(active == 1, "T09 queued uninstall preserves the one-active-operation invariant");
    simulation.action("job:fail:job-1:download"); simulation.action("job:fail:job-2:download"); simulation.action("job:fail:job-3:download");
    simulation.tick(3);
    const auto removed = std::find_if(simulation.state().installations.begin(), simulation.state().installations.end(),
                                      [](const z3cloudplay::homebrew::Installation& value) { return value.game_id == 900001; });
    check(removed != simulation.state().installations.end() && !removed->available && removed->saves_present,
          "T09 uninstall removes only the selected fixture install and preserves saves");
    check(simulation.action("reset"), "T09 reset is available");
    const auto after_reset = std::find_if(simulation.state().installations.begin(), simulation.state().installations.end(),
                                          [](const z3cloudplay::homebrew::Installation& value) { return value.game_id == 900001; });
    check(after_reset != simulation.state().installations.end() && after_reset->saves_present,
          "T09 reset preserves fixture installs and saves");
    check(simulation.action("install:900001:external"), "T09 a completed uninstall does not block reinstall");
    check(std::any_of(simulation.state().jobs.begin(), simulation.state().jobs.end(),
                      [](const z3cloudplay::homebrew::Job& job) { return job.game_id == 900001 && job.kind == "install" && job.state == z3cloudplay::homebrew::JobState::Queued; }),
          "T09 reinstall creates a fresh stable job");
}

void t10_session_failure_running_stop_and_crash(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("launch-failure");
    activate(simulation, "session-retry");
    simulation.tick(2);
    check(simulation.state().session == z3cloudplay::homebrew::SessionState::Running, "T10 launch retry reaches running");
    check(simulation.action("session:quit"), "T10 session quit is controller reachable");
    simulation.action("inject:stop-failure");
    check(simulation.state().session == z3cloudplay::homebrew::SessionState::Failed, "T10 stop failure is recoverable");
    simulation.action("inject:crash");
    activate(simulation, "session-logs");
    simulation.action("circle");
    check(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Details
              && simulation.state().navigation.detail_id == 900001,
          "T10 crash logs return to prior game details");
}

void t11_preferences_failure_and_controller_modal_recovery(const std::filesystem::path& fixtures,
                                                            const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:settings");
    activate(simulation, "settings-motion");
    z3cloudplay::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().reduced_motion, "T11 reduced motion persists across restart");
    restored.scenario("save-failure");
    restored.action("route:library");
    restored.action("triangle");
    const auto focus = restored.state().navigation.focus_key;
    restored.action("inject:controller-disconnect");
    restored.action("inject:controller-reconnect");
    check(restored.view().surface == "filters" && restored.state().navigation.focus_key == focus,
          "T11 controller reconnect retains modal and visible focus");
    restored.action("circle"); restored.action("route:settings");
    activate(restored, "settings-notices"); activate(restored, "retry-save"); restored.action("circle");
    activate(restored, "settings-logs");
    check(restored.state().log_state == z3cloudplay::homebrew::LogState::Loading, "T11 logs expose a loading state");
    restored.tick(1);
    check(restored.state().log_state == z3cloudplay::homebrew::LogState::Ready && restored.state().log_lines.size() == 32,
          "T11 logs resolve deterministically to bounded contextual records");
    for (int index = 0; index < 20; ++index) restored.action("r2");
    check(restored.state().log_offset == 24 && restored.state().log_lines.size() <= 128,
          "T11 log scrolling clamps to the eight-row viewport");
    restored.action("inject:logs-failure"); activate(restored, "logs-retry"); restored.tick(1);
    check(restored.state().log_state == z3cloudplay::homebrew::LogState::Ready, "T11 failed logs retry through normal actions");
    restored.action("circle"); activate(restored, "settings-account"); activate(restored, "account-switch");
    check(restored.state().navigation.modals.back().focused_action == 0, "T11 account switch defaults to Cancel");
    activate(restored, "confirm-destructive");
    check(restored.state().account.authentication == z3cloudplay::homebrew::AuthState::Waiting
              && restored.state().account.id.empty() && restored.state().library.favorites.empty(),
          "T11 account switch clears identity and account-visible cache");
    restored.action("inject:qr-approved"); restored.tick(1);
    check(restored.state().account.authentication == z3cloudplay::homebrew::AuthState::SignedIn
              && !restored.state().account.id.empty() && restored.state().account.last_sync > 0,
          "T11 account recovery restores a new fixture identity and last-sync state");
    restored.action("route:library"); restored.action("search"); restored.text("Night Orchard"); restored.action("done"); restored.action("square");
    const auto switched_id = restored.state().account.id;
    const auto switched_name = restored.state().account.display_name;
    const auto switched_sync = restored.state().account.last_sync;
    z3cloudplay::homebrew::Simulation switched_restart(fixtures, profile);
    check(switched_restart.state().account.id == switched_id && switched_restart.state().account.display_name == switched_name
              && switched_restart.state().account.last_sync == switched_sync
              && switched_restart.state().library.query == "Night Orchard"
              && switched_restart.state().library.favorites.contains(900010),
          "T11 switched account identity and organization restore only from its profile");
    restored.action("route:settings"); activate(restored, "settings-about");
    check(restored.view().surface == "about" && restored.view().rows.size() == 6,
          "T11 About exposes build, framework, platform, kit, kit source, and Back rows");
    {
        const auto& rows = restored.view().rows;
        const auto kit = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.key == "about-kit"; });
        check(kit != rows.end() && kit->value.rfind("hl2-fw1360-dev1 · ", 0) == 0 &&
                  kit->subtitle == "The kit this version of Z3CloudPlay was tested with",
              "T11 About names the compatibility kit and whether it is the tested one");
    }
}

void t12_stress_dataset_keeps_logical_focus(const std::filesystem::path& fixtures,
                                            const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("delayed-art");
    const auto delayed_id = simulation.state().navigation.selected_game_id;
    const auto delayed_index = simulation.state().navigation.selected_index;
    const auto delayed_scroll = simulation.state().navigation.scroll_offsets.at("library-grid");
    const auto bytes = simulation.state().jobs.front().completed_bytes;
    const auto delayed = std::find_if(simulation.state().games.begin(), simulation.state().games.end(),
                                      [](const z3cloudplay::homebrew::Game& game) { return game.id == 900007; });
    check(delayed != simulation.state().games.end() && delayed->art_pending, "T12 delayed art exposes a stable placeholder state");
    simulation.tick(1);
    check(!delayed->art_pending && simulation.state().navigation.selected_game_id == delayed_id
              && simulation.state().navigation.selected_index == delayed_index
              && simulation.state().navigation.scroll_offsets.at("library-grid") == delayed_scroll
              && simulation.state().jobs.front().completed_bytes > bytes,
          "T12 asynchronous art and job updates preserve focus and viewport");
    simulation.scenario("stress");
    check(simulation.state().games.size() == 1000, "T12 stress scenario loads all 1,000 logical games");
    check(simulation.view().rows.size() <= 24, "T12 library materializes a bounded window");
    simulation.action("select:100500");
    const auto focused = simulation.state().navigation.selected_game_id;
    simulation.tick(3);
    check(simulation.state().navigation.selected_game_id == focused,
          "T12 asynchronous art/job updates never steal stable game focus");
    check(simulation.snapshot().find("\"materialized_row_count\":24") != std::string::npos,
          "T12 snapshot records bounded library materialization");
}

void t13_settings_sections_rail(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:settings");
    const auto section = [&simulation] {
        const auto& view = simulation.view();
        return view.rows[static_cast<std::size_t>(view.focused_row)].section;
    };
    check(section() == "Account", "T13 Settings opens on the Account section");
    simulation.action("left");
    check(simulation.state().navigation.settings_rail_focused, "T13 Left moves to the section rail");
    simulation.action("down");
    check(section() == "Library" && simulation.state().navigation.focus_key == "settings-storage",
          "T13 Down on the rail switches to the next section's first row");
    simulation.action("cross");
    check(!simulation.state().navigation.settings_rail_focused && simulation.state().navigation.focus_key == "settings-storage",
          "T13 Cross on the rail enters the section without activating its row");
    simulation.action("down");
    simulation.action("down");
    check(section() == "Display" && simulation.state().navigation.focus_key == "settings-motion",
          "T13 Down past a section's last row continues into the next section");
    simulation.action("left");
    simulation.action("right");
    check(!simulation.state().navigation.settings_rail_focused && simulation.state().navigation.focus_key == "settings-motion",
          "T13 Right returns from the rail to the same row");

    // Quit Z3CloudPlay, as on macOS: the rail entry is the action, with no "Quit / Review" card.
    simulation.action("left");
    for (int step = 0; step < 12; ++step) simulation.action("down");
    check(section() == "Quit Z3CloudPlay" && simulation.state().navigation.focus_key == "settings-quit",
          "T13 the last rail entry is Quit Z3CloudPlay");
    simulation.action("right");
    check(simulation.state().navigation.settings_rail_focused, "T13 Right stays on the Quit Z3CloudPlay rail entry");
    simulation.action("cross");
    check(!simulation.state().navigation.modals.empty() && simulation.state().navigation.modals.back().id == "confirmation",
          "T13 Cross on Quit Z3CloudPlay asks to confirm quitting");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty() && simulation.state().navigation.settings_rail_focused,
          "T13 cancelling returns to the Quit Z3CloudPlay rail entry");
    simulation.action("up");
    simulation.action("right");
    for (int step = 0; step < 20; ++step) simulation.action("down");
    check(section() != "Quit Z3CloudPlay", "T13 moving down the rows never lands on the Quit Z3CloudPlay entry");
}

} // namespace

void t14_tab_bar_from_the_top_of_a_page(const std::filesystem::path& fixtures,
                                        const std::filesystem::path& profile) {
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("default");
    simulation.action("r1");
    check(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Library, "T14 Library is open");
    for (int step = 0; step < 20 && !simulation.state().navigation.tabs_focused; ++step) simulation.action("up");
    check(simulation.state().navigation.tabs_focused && simulation.state().navigation.tab_focus == 1,
          "T14 Up from the top of a page focuses the current tab");
    simulation.action("right");
    simulation.action("right");
    check(simulation.state().navigation.tab_focus == 3 &&
              simulation.state().navigation.route == z3cloudplay::homebrew::Route::Library,
          "T14 Left/Right move between tabs without opening them");
    simulation.action("right");
    simulation.action("right");
    check(simulation.state().navigation.tab_focus == 4, "T14 the last tab stays focused at the end");
    simulation.action("cross");
    check(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Settings &&
              !simulation.state().navigation.tabs_focused, "T14 Cross opens the focused tab and focuses its page");
    for (int step = 0; step < 20 && !simulation.state().navigation.tabs_focused; ++step) simulation.action("up");
    simulation.action("left");
    simulation.action("down");
    check(!simulation.state().navigation.tabs_focused &&
              simulation.state().navigation.route == z3cloudplay::homebrew::Route::Settings,
          "T14 Down returns to the page without changing tab");
    for (int step = 0; step < 20 && !simulation.state().navigation.tabs_focused; ++step) simulation.action("up");
    simulation.action("circle");
    check(!simulation.state().navigation.tabs_focused, "T14 Circle returns to the page");
}

// Issue 52: every value a recipe sets has a row the player can change and reset.
void t52_recipe_values_in_game_settings(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = z3cloudplay::runtime;
    z3cloudplay::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    std::map<z3cloudplay::GameId, z3cloudplay::homebrew::LiveGameSetup> setups;
    auto& setup = setups[900001];
    setup.scanned = true;
    setup.programs = {{"bin/win64/hl2_win64.exe", "pe64", "", false, "d3d11 graphics=dxvk"},
                      {"Launcher.exe", "pe32", "This game needs .NET, which Z3CloudPlay's runtime doesn't include yet.", true, ""},
                      {"Stub.exe", "pe32", "This game's program has Steam DRM in a form Z3CloudPlay can't unpack yet.", false, ""}};
    // The programs show in an open Program picker as soon as the folder's scan arrives.
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "setting-launch_option");
    check(!find_row(simulation, "program-0"), "T52 no programs before the scan");
    simulation.SetGameSetups(setups);
    check(find_row(simulation, "program-0") && simulation.state().navigation.modals.back().setting_id == "launch_option",
          "T52 the open picker lists the scanned programs");
    simulation.action("circle");
    simulation.action("circle");
    simulation.action("circle");
    const auto overrides = [&]() -> const std::map<std::string, std::string>& {
        return simulation.state().game_settings.at(900001).overrides;
    };
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "section-advanced");
    check(find_row(simulation, "setting-launch_option")->title == "Program", "T52 the launch option is the Program row");

    // Program: Steam's entries are suggestions; any program in the game folder can start.
    activate(simulation, "setting-launch_option");
    check(find_row(simulation, "choice-1") && find_row(simulation, "program-0") && find_row(simulation, "value-reset"),
          "T52 the Program picker lists launch options and the folder's programs");
    check(!find_row(simulation, "program-2")->enabled && find_row(simulation, "program-1")->enabled,
          "T52 a hard refusal cannot be picked, a soft one can");
    activate(simulation, "program-0");
    check(overrides().at("program") == "bin/win64/hl2_win64.exe" && overrides().at("architecture") == "pe64",
          "T52 a picked program saves with its architecture");
    check(find_row(simulation, "setting-launch_option")->value == "hl2_win64.exe" &&
          find_row(simulation, "setting-architecture")->value == "64-bit" && !find_row(simulation, "setting-architecture")->enabled,
          "T52 the program type follows the program");
    check(find_row(simulation, "effective-command")->subtitle.rfind("bin/win64/hl2_win64.exe -game hl2", 0) == 0,
          "T52 the effective command starts the picked program");
    check(!find_row(simulation, "setting-start_check"), "T52 no known problem, no Try anyway");
    // A preset or a reset keeps the program, so a game that starts only with it keeps starting.
    check(simulation.action("focus:setting-architecture") && simulation.action("triangle"), "T52 triangle on program type");
    check(overrides().count("architecture"), "T52 the program type is not reset on its own");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-0");
    activate(simulation, "preset-picker");
    activate(simulation, "preset-mouse-driven");
    check(overrides().count("program") && overrides().count("architecture") && !overrides().count("resolution"),
          "T52 choosing a preset keeps the program");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-0");
    activate(simulation, "reset-settings");
    check(overrides().count("program") && !overrides().count("resolution"), "T52 reset keeps the program");
    check(!find_row(simulation, "reset-settings")->enabled, "T52 nothing else left to reset");
    activate(simulation, "setting-launch_option");
    activate(simulation, "value-reset");
    check(!overrides().count("program") && !overrides().count("architecture"), "T52 reset returns to the recipe's program");
    // A soft refusal: picking it means trying anyway, and the reason stays on the row.
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-1");
    check(overrides().at("start_check") == "try", "T52 a soft-refused program tries anyway");
    activate(simulation, "section-more");
    const auto* problems = find_row(simulation, "setting-start_check");
    check(problems && problems->value == "Try anyway" && problems->subtitle.find(".NET") != std::string::npos,
          "T52 Known problems shows the refusal");
    check(simulation.action("focus:setting-launch_option") && simulation.action("triangle"), "T52 triangle resets the program");
    check(!overrides().count("program") && !overrides().count("architecture"), "T52 triangle drops the picked program");
    check(!overrides().count("start_check"), "T52 Try anyway goes with the program it was set for");
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-1");
    activate(simulation, "setting-launch_option");
    activate(simulation, "value-reset");
    check(!overrides().count("start_check") && !overrides().count("program"), "T52 reset to the recipe drops Try anyway");
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-1");
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-0");
    check(!overrides().count("start_check"), "T52 another program drops Try anyway");
    activate(simulation, "setting-launch_option");
    activate(simulation, "value-reset");

    // Required arguments: start as the recipe's, can be replaced and reset.
    activate(simulation, "setting-arguments");
    check(simulation.state().navigation.modals.back().id == "setting-value", "T52 required arguments open their actions");
    check(!find_row(simulation, "value-reset")->enabled, "T52 nothing to reset yet");
    activate(simulation, "value-edit");
    check(simulation.state().editor.value.rfind("-game hl2 -steam {resolution}", 0) == 0, "T52 the editor starts from the recipe");
    simulation.action("triangle");  // clears the editor
    simulation.text("-game hl2 {resolution}");
    simulation.action("done");
    check(overrides().at("arguments") == "-game hl2 {resolution}", "T52 required arguments save");
    check(find_row(simulation, "effective-command")->subtitle == "hl2.exe -game hl2 -w 1920 -h 1080", "T52 the command follows");
    activate(simulation, "setting-arguments");
    activate(simulation, "value-reset");
    check(!overrides().count("arguments"), "T52 required arguments reset");

    // Library overrides: the whole list, each entry removable, then added to and reset.
    activate(simulation, "setting-dll_overrides");
    check(find_row(simulation, "override-remove-0")->title == "d3d9=n" && !find_row(simulation, "override-remove-1"),
          "T52 the recipe's overrides are listed");
    activate(simulation, "override-remove-0");
    check(overrides().at("dll_overrides").empty(), "T52 a recipe override can be removed");
    activate(simulation, "override-add");
    simulation.text("dinput8=n,b");
    simulation.action("done");
    check(overrides().at("dll_overrides") == "dinput8=n,b", "T52 an override can be added");
    activate(simulation, "setting-dll_overrides");
    activate(simulation, "value-reset");
    check(!overrides().count("dll_overrides"), "T52 library overrides reset");

    // Locked rows: Change anyway, then reset to the recipe.
    activate(simulation, "setting-graphics");
    activate(simulation, "unlock-setting");
    check(simulation.state().navigation.modals.back().setting_id == "graphics", "T52 Change anyway opens the picker");
    activate(simulation, "choice-3");
    check(overrides().at("graphics") == "opengl" && overrides().count("unlock.graphics"), "T52 a locked row changes anyway");
    check(find_row(simulation, "setting-graphics")->badge == "Changed anyway" &&
          find_row(simulation, "setting-graphics")->subtitle.find("may not start") != std::string::npos,
          "T52 the warning stays on the row");
    activate(simulation, "setting-graphics");
    activate(simulation, "value-reset");
    check(!overrides().count("graphics") && !overrides().count("unlock.graphics"), "T52 reset locks the row again");

    // Export as recipe: a community recipe file that reads back.
    const auto directory = std::filesystem::temp_directory_path() / ("z3cloudplay-t52-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    simulation.SetRecipeExportTarget(directory, "13.60");
    activate(simulation, "export-recipe");
    check(simulation.state().navigation.modals.back().title == "Recipe exported", "T52 export says where it went");
    rt::CommunityRecipe community;
    std::string reason;
    check(rt::ParseCommunityRecipe(read_text(directory / "recipe-900001.json"), &community, &reason) &&
              community.tested_firmware == "13.60" && community.executable == "hl2.exe" &&
              community.recipe.dll_overrides == "d3d9=n" && community.recipe.locked.count("graphics"),
          "T52 the exported recipe reads back");
    std::filesystem::remove_all(directory);

    // A game Z3CloudPlay refused starts its sheet from the draft recipe.
    rt::GameRecipe draft;
    draft.id = rt::GeneratedRecipeId(900003);
    draft.version = 2;
    draft.app_id = 900003;
    setups[900003].draft = draft;
    setups[900003].refusal_soft = true;
    setups[900003].refusal = "Steam lists no Windows program this console can start for this game.";
    simulation.SetGameSetups(setups);
    rt::EffectiveSettings values;
    check(simulation.RecipeFor(900003).id == draft.id && simulation.EffectiveSettingsFor(900003, &values) && !values.launch,
          "T52 a refused game's sheet has no program until the player picks one");
    check(simulation.known_problem(900003), "T52 a refused game shows Known problems");
}

int main(int argc, char** argv) {
    check(argc == 2, "fixture directory argument is required");
    const std::filesystem::path fixtures(argv[1]);
    const auto profile = temp_profile();
    auto clean = [&] { std::error_code error; std::filesystem::remove(profile, error); };
    clean();
    t01_authentication_ignores_late_approval(fixtures, profile);
    clean();
    t02_focus_and_nested_modal_restoration(fixtures, profile);
    clean();
    t03_search_filters_and_focus_reconciliation(fixtures, profile);
    clean();
    t03_circle_leaves_an_applied_search(fixtures, profile);
    clean();
    t04_organization_persists_without_deleting_games(fixtures, profile);
    clean();
    t05_install_rechecks_storage_and_submits_once(fixtures, profile);
    clean();
    t06_queue_transitions_are_single_active_and_safe(fixtures, profile);
    clean();
    t07_interruption_and_verification_recovery(fixtures, profile);
    clean();
    t08_nested_settings_restore_openers(fixtures, profile);
    clean();
    t08b_settings_sheet_rules(fixtures, profile);
    settings_revision_exhaustion_preserves_record(fixtures, profile);
    settings_sheet_details(fixtures, profile);
    settings_save_failure_does_not_claim_success(fixtures, profile);
    clean();
    t52_recipe_values_in_game_settings(fixtures, profile);
    clean();
    t09_uninstall_and_reset_preserve_declared_data(fixtures, profile);
    clean();
    t10_session_failure_running_stop_and_crash(fixtures, profile);
    clean();
    t11_preferences_failure_and_controller_modal_recovery(fixtures, profile);
    clean();
    t12_stress_dataset_keeps_logical_focus(fixtures, profile);
    clean();
    t13_settings_sections_rail(fixtures, profile);
    clean();
    t14_tab_bar_from_the_top_of_a_page(fixtures, profile);
    clean();
    public_contract_and_snapshot(fixtures, profile);
    std::error_code error;
    std::filesystem::remove(profile, error);
    std::cout << "homebrew_simulation_test passed\n";
}
