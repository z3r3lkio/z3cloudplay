// homebrew_library_model_test.cpp - Real library projection tests.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#include "homebrew/simulation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

using z3cloudplay::GameSummary;
using z3cloudplay::homebrew::AuthState;
using z3cloudplay::homebrew::CloudRequestKind;
using z3cloudplay::homebrew::CloudStatus;
using z3cloudplay::homebrew::LibraryUpdate;
using z3cloudplay::homebrew::ModelMode;
using z3cloudplay::homebrew::Route;
using z3cloudplay::homebrew::RowKind;
using z3cloudplay::homebrew::ServiceAction;
using z3cloudplay::homebrew::Simulation;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Simulation make_live() {
    return Simulation("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
}

GameSummary game(z3cloudplay::GameId id, std::string title, std::string art = {},
                 std::string description = {}) {
    GameSummary result;
    result.id = id;
    result.title = std::move(title);
    result.artwork_source = std::move(art);
    result.description = std::move(description);
    return result;
}

const z3cloudplay::homebrew::Game* find_game(const Simulation& model, z3cloudplay::GameId id) {
    const auto found = std::find_if(model.state().games.begin(), model.state().games.end(),
                                    [id](const auto& item) { return item.id == id; });
    return found == model.state().games.end() ? nullptr : &*found;
}

bool has_row(const Simulation& model, const std::string& key) {
    return std::any_of(model.view().rows.begin(), model.view().rows.end(),
                       [&key](const auto& row) { return row.key == key; });
}

void activate(Simulation& model, const std::string& key) {
    check(model.action("focus:" + key), "requested row exists");
    check(model.action("cross"), "requested row activates");
}

void projects_only_owned_facts_and_delivery_status() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});

    auto summary = game(10, "Known title", "https://cdn.invalid/10.jpg", "Known description");
    summary.developer = "Not projected by this boundary";
    summary.playtime_minutes = 91;
    summary.last_played_unix_seconds = 4'102'444'800ULL;
    check(model.UpdateLibrary({{summary}, false, {}, false, true, 1234}),
          "valid successful live snapshot is accepted");

    const auto* projected = find_game(model, 10);
    check(projected != nullptr && projected->title == "Known title"
              && projected->description == "Known description"
              && projected->source == "Steam"
              && projected->artwork_source == "https://cdn.invalid/10.jpg",
          "shared identity, text, artwork and known storefront are projected");
    check(projected->playtime_minutes == 91 && projected->last_played == 4'102'444'800LL,
          "owned play history is projected without narrowing valid timestamps");
    check(projected->genres.empty() && projected->controller_support.empty()
              && !projected->download_bytes && !projected->install_bytes
              && !projected->added_at && !projected->art_pending && !projected->art_failed,
          "projection does not invent install, controller, metadata or artwork state");
    check(model.state().account.authentication == AuthState::SignedIn
              && model.state().account.cache_available
              && model.state().account.last_sync == 1234
              && !model.state().library_loading && !model.state().library_syncing
              && !model.state().library_offline && model.state().account.error.empty(),
          "successful delivery updates library status without repurposing authentication");
}

void loading_and_failure_preserve_last_good_snapshot() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(1, "One"), game(2, "Two")}, false, {}, false, true, 100}),
          "initial snapshot accepted");

    check(model.UpdateLibrary({{}, true, {}, false, true, std::nullopt}),
          "loading delivery status accepted");
    check(model.state().games.size() == 2 && model.state().library_loading
              && model.state().library_syncing && model.state().account.last_sync == 100,
          "loading preserves games and prior retrieval time");

    check(model.UpdateLibrary({{}, false, "Refresh failed", true, true, std::nullopt}),
          "failure delivery status accepted");
    check(model.state().games.size() == 2 && !model.state().library_loading
              && !model.state().library_syncing && model.state().library_offline
              && model.state().account.error == "Refresh failed",
          "failed refresh keeps the last good catalog and exposes stale error state");

    check(model.UpdateLibrary({{}, false, {}, false, true, 200}),
          "genuine successful empty snapshot accepted");
    check(model.state().games.empty() && model.state().account.last_sync == 200
              && model.state().account.error.empty() && !model.state().library_offline,
          "successful empty ownership replaces stale games and clears failure state");

    check(model.UpdateLibrary({{game(3, "Three")}, false, {}, false, true, 201})
              && model.action("details:3"),
          "details can be open before an empty ownership result");
    check(model.UpdateLibrary({{}, false, {}, false, true, 202})
              && model.state().navigation.route == Route::Home
              && !model.state().navigation.detail_id
              && model.state().navigation.selected_game_id == 0
              && has_row(model, "home-library"),
          "empty ownership leaves a removed details record for its empty origin surface");
}

void rejects_invalid_snapshots_transactionally_and_leaves_simulation_alone() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(5, "Five")}, false, {}, false, true, 50}),
          "baseline snapshot accepted");

    const auto assert_unchanged = [&model](const char* message) {
        check(model.state().games.size() == 1 && model.state().games.front().id == 5
                  && model.state().account.last_sync == 50
                  && model.state().account.cache_available
                  && model.state().account.error.empty() && !model.state().library_offline,
              message);
    };
    check(!model.UpdateLibrary({{game(0, "Zero")}, false, "must not leak", true, false, 60}),
          "zero app ID is rejected");
    assert_unchanged("zero-ID rejection is transactional");
    check(!model.UpdateLibrary({{game(6, "")}, false, {}, false, false, 60}),
          "empty title is rejected");
    assert_unchanged("empty-title rejection is transactional");
    check(!model.UpdateLibrary({{game(6, "First"), game(6, "Duplicate")}, false, {}, false, false, 60}),
          "duplicate app ID is rejected");
    assert_unchanged("duplicate rejection is transactional");

    Simulation simulation("fixtures-not-used", "profile-not-used.json", ModelMode::Simulation);
    const auto prior_games = simulation.state().games.size();
    check(!simulation.UpdateLibrary({{game(88, "Live only")}, false, {}, false, true, 90})
              && simulation.state().games.size() == prior_games,
          "production library API cannot mutate deterministic simulation state");
}

void preserves_identity_and_scroll_then_chooses_a_stable_neighbor() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    std::vector<GameSummary> initial;
    for (z3cloudplay::GameId id = 1; id <= 30; ++id)
        initial.push_back(game(id, "Game " + (id < 10 ? std::string("0") : std::string{}) + std::to_string(id)));
    check(model.UpdateLibrary({initial, false, {}, false, true, 1}), "large snapshot accepted");
    check(model.action("r1") && model.state().navigation.route == Route::Library,
          "navigation enters library");
    check(model.action("select:13") && model.action("down"), "selection moves into a scrolled row");
    const auto selected_id = model.state().navigation.selected_game_id;
    const auto scroll = model.state().navigation.scroll_offsets.at("library-grid");
    check(selected_id == 19 && scroll == 3, "test starts with stable identity and nonzero scroll");

    auto reordered = initial;
    for (auto& item : reordered)
        if (item.id == selected_id) item.title = "AAA selected moved";
    std::reverse(reordered.begin(), reordered.end());
    check(model.UpdateLibrary({reordered, false, {}, false, true, 2}), "reordered snapshot accepted");
    check(model.state().navigation.selected_game_id == selected_id
              && model.state().navigation.focus_key == "game-19"
              && model.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "refresh preserves selected Steam ID, focus and scroll across reorder");

    const int prior_index = model.state().navigation.selected_index;
    reordered.erase(std::remove_if(reordered.begin(), reordered.end(), [selected_id](const auto& item) {
        return item.id == selected_id;
    }), reordered.end());
    check(model.UpdateLibrary({reordered, false, {}, false, true, 3}), "removal snapshot accepted");
    const auto visible = model.visible_games();
    const int expected_index = std::min(prior_index, static_cast<int>(visible.size()) - 1);
    check(!visible.empty() && model.state().navigation.selected_index == expected_index
              && model.state().navigation.selected_game_id == visible[static_cast<std::size_t>(expected_index)]->id
              && model.state().navigation.focus_key == "game-" + std::to_string(model.state().navigation.selected_game_id)
              && model.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "removed selection chooses the item at the prior visible index and preserves scroll");

    const auto detail_id = model.state().navigation.selected_game_id;
    check(model.action("details:" + std::to_string(detail_id))
              && model.action("focus:details-more"),
          "selected neighbor opens with a stable details focus");
    auto detail_refresh = reordered;
    for (auto& item : detail_refresh) if (item.id == detail_id) item.title = "AAA detail moved";
    check(model.UpdateLibrary({detail_refresh, false, {}, false, true, 4}),
          "details refresh accepted");
    check(model.state().navigation.detail_id == detail_id
              && model.state().navigation.selected_game_id == detail_id
              && model.state().navigation.focus_key == "details-more"
              && model.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "details refresh preserves game identity, action focus and scroll by Steam ID");
}

void live_details_keep_personal_edits_and_block_fixture_operations() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(7, "Seven")}, false, {}, false, true, 1}),
          "live game delivered");
    check(model.action("select:7") && model.action("square")
              && model.action("note:7:personal note")
              && model.action("collection:create:Personal")
              && model.action("collection:add:collection-1:7"),
          "favorites, notes and collections remain editable in live mode");
    check(model.UpdateLibrary({{game(8, "Eight"), game(7, "Seven renamed")}, false, {}, false, true, 2}),
          "follow-up live snapshot accepted");
    check(model.state().library.favorites.contains(7)
              && model.state().library.notes.at(7) == "personal note"
              && model.state().library.collections.front().members.contains(7),
          "refresh preserves account-scoped personal annotations");

    check(model.action("details:7"), "live details open");
    check(has_row(model, "details-favorite")
              && std::none_of(model.view().rows.begin(), model.view().rows.end(),
                              [](const auto& row) { return row.key == "details-install" && row.enabled; })
              && !has_row(model, "details-play")
              && !has_row(model, "details-simulate-launch"),
          "live details expose personal actions without fixture install/session actions");
    const auto focused = model.view().focused_row;
    check(focused >= 0 && model.view().rows[static_cast<std::size_t>(focused)].enabled
              && model.view().rows[static_cast<std::size_t>(focused)].key == "details-favorite",
          "live details enter on a visible enabled favorite action");
    check(model.action("focus:details-more") && model.action("cross"), "personal context opens");
    check(has_row(model, "favorite") && has_row(model, "collection") && has_row(model, "note")
              && has_row(model, "hide") && !has_row(model, "verify") && !has_row(model, "logs"),
          "live context retains personal actions and hides fixture operations");
    const auto uninstall = std::find_if(model.view().rows.begin(), model.view().rows.end(),
                                        [](const auto& row) { return row.key == "uninstall"; });
    check(uninstall != model.view().rows.end() && !uninstall->enabled, "live Uninstall is disabled until installed");
}

const z3cloudplay::homebrew::ViewRow* find_row(const Simulation& model, const std::string& key) {
    const auto found = std::find_if(model.view().rows.begin(), model.view().rows.end(),
                                    [&key](const auto& row) { return row.key == key; });
    return found == model.view().rows.end() ? nullptr : &*found;
}

void live_install_requests_and_projection() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(42,"Synthetic")},false,{},false,false,{}}),"library loaded");
    check(model.action("details:42"), "details open");
    check(has_row(model, "details-checking"), "install state waits for Steam details");
    {
        const auto* waiting = find_row(model, "details-install");
        check(waiting && !waiting->enabled && waiting->value == "…", "Install stays in place, disabled, while Steam answers");
    }
    check(model.UpdateAppDetails(42, 300'000'000, 600'000'000, true), "app details apply");
    check(has_row(model, "details-install"), "an installable game offers Install");
    check(model.state().navigation.focus_key == "details-install",
          "focus moves from Favorite to Install once Steam's details arrive");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    check(find_row(model, "details-install")->value == "600 MB",
          "Install shows the install size (not the download size), the one size a game has");
    check(find_row(model, "details-install")->subtitle == "To Console storage · 1.0 GB free",
          "Install names the destination and its free space");
    activate(model, "details-install");
    auto requests = model.TakeInstallRequests();
    check(model.view().surface != "install", "a game that fits installs without a confirmation sheet");
    check(requests.size() == 1 && requests[0].kind == z3cloudplay::homebrew::InstallRequestKind::Install && requests[0].game_id == 42,
          "Install sends one request");
    model.UpdateInstalls({{42, z3cloudplay::homebrew::JobState::Downloading, 50, 200, {}}}, {});
    check(has_row(model, "details-download") && !has_row(model, "details-install"), "a running job replaces Install");
    check(model.state().navigation.focus_key == "details-download", "focus follows Install to the download");
    activate(model, "details-download");
    check(model.state().navigation.route == z3cloudplay::homebrew::Route::Downloads, "the download row opens Downloads");
    activate(model, "job-42");
    activate(model, "job-pause-42");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == z3cloudplay::homebrew::InstallRequestKind::Pause, "Pause goes to the service");
    model.UpdateInstalls({}, {{42, 600'000'000}});
    check(model.action("details:42") && has_row(model, "details-installed"), "a finished install shows Installed");
    check(!has_row(model, "details-runtime"), "no runtime row before the runtime host reports");
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready,
                        {{42, "Not prepared", "Makes a private bottle and its own PS5 title", true, false}});
    check(model.action("details:42") && has_row(model, "details-runtime"), "an installed game offers Prepare");
    const auto* play = find_row(model, "details-play");
    check(play && play->enabled && play->action == "runtime-play", "Play is offered before the game is prepared");
    activate(model, "details-play");
    const auto plays = model.TakeRuntimePlayRequests();
    check(plays.size() == 1 && plays[0] == 42, "Play on an unprepared game goes to the controller, which prepares first");
    // The PS5 title's state is a note under the install line, not a second button: Play prepares.
    const auto* runtime_note = find_row(model, "details-runtime");
    check(runtime_note && runtime_note->kind == z3cloudplay::homebrew::RowKind::Status && runtime_note->action.empty() &&
              runtime_note->value == "Not prepared",
          "the PS5 title state is a note");
    model.RequestRuntimePrepare(42);
    check(model.TakeRuntimePrepareRequests().size() == 1, "Prepare can be queued without a button press");
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {{42, "Preparing", "Copying the bottle", false, false}});
    check(model.action("details:42"), "details reopen");
    check(find_row(model, "details-play")->progress == 0.0F && find_row(model, "details-runtime")->progress == 0.0F,
          "a step without a known fraction shows no bar");
    {
        z3cloudplay::homebrew::LiveRuntimeGame copying{42, "Preparing", "Copying the Windows environment", false, false};
        copying.progress = 0.42F;
        model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {copying});
        check(find_row(model, "details-play")->progress == 0.42F && find_row(model, "details-runtime")->progress == 0.42F,
              "a copy with a known fraction shows it on Play and the PS5 title");
        model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {{42, "Preparing", "Copying the bottle", false, false}});
    }
    check(model.state().installations.size() == 1 && !model.state().installations[0].runtime_ready, "preparing is not ready");
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {{42, "Ready", "FAKE78001", false, true}});
    model.UpdateInstalls({}, {{42, 600'000'000}});
    check(model.state().installations[0].runtime_ready, "readiness survives install refreshes");
    check(model.action("details:42"), "details reopen");
    check(model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    activate(model, "confirm-destructive");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == z3cloudplay::homebrew::InstallRequestKind::Uninstall && requests[0].game_id == 42,
          "Uninstall confirms and goes to the service");
    model.UpdateAppDetails(42, std::nullopt, std::nullopt, false);
    model.UpdateInstalls({}, {});
    check(model.action("details:42") && has_row(model, "details-unavailable"), "a game without Windows content says so");
}

void live_install_volumes() {
    using z3cloudplay::homebrew::InstallRequestKind;
    using z3cloudplay::homebrew::JobState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Installed"), game(43, "Downloading"), game(44, "On USB"), game(45, "New")},
                               false, {}, false, false, {}}), "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, std::nullopt, std::nullopt},
                         {"m2", "M.2 SSD", true, 2'000, 500},
                         {"usb0", "USB drive", false, std::nullopt, std::nullopt}}, "internal");
    model.UpdateAppDetails(43, 400, 800, true);
    model.UpdateInstalls({{43, JobState::Downloading, 100, 400, {}, "m2"}},
                         {{42, 600, "internal", true}, {44, 0, "usb0", false}});

    const auto& volumes = model.state().volumes;
    check(volumes.size() == 3 && model.state().default_volume_id == "internal", "volumes and default projected");
    check(!volumes[0].capacity_known && volumes[0].games_bytes == 600 && volumes[0].used_bytes == 600
              && volumes[0].reserved_bytes == 0, "a drive without a space report counts finished installs");
    check(volumes[1].capacity_known && volumes[1].capacity_bytes == 2'000 && volumes[1].used_bytes == 1'500
              && volumes[1].reserved_bytes == 800, "a reported drive uses its own figures plus full install reservations");
    check(!volumes[2].available, "an absent drive stays listed as unavailable");

    // Settings shows the default and changes it through the install service.
    model.action("l1");
    const auto* storage = find_row(model, "settings-storage");
    check(storage && storage->enabled && storage->value == "Console storage", "Settings shows the default storage");
    activate(model, "settings-storage");
    const auto* usb = find_row(model, "volume-usb0");
    check(has_row(model, "volume-internal") && has_row(model, "volume-m2") && usb && !usb->enabled,
          "the picker lists every drive and disables an absent one");
    const auto* m2 = find_row(model, "volume-m2");
    check(m2 && m2->value.find("free") != std::string::npos, "a reported drive shows its free space");
    activate(model, "volume-m2");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::SetDefaultVolume && requests[0].volume_id == "m2",
          "choosing a drive asks the install service to use it");
    check(model.state().default_volume_id == "m2", "the choice shows immediately");
    activate(model, "settings-storage");
    activate(model, "volume-m2");
    check(model.TakeInstallRequests().empty(), "choosing the current default sends nothing");

    // Details show where a game is and where a new install goes.
    check(model.action("details:42"), "installed game opens");
    const auto* installed = find_row(model, "details-installed");
    check(installed && installed->title == "Installed" && installed->subtitle == "On Console storage", "an installed game names its drive");
    check(model.action("details:44"), "game on an absent drive opens");
    installed = find_row(model, "details-installed");
    check(installed && installed->title == "Storage disconnected" && installed->subtitle == "Connect USB drive to use this game",
          "a game on an absent drive says so even without a known size");
    check(model.action("details:45") && model.UpdateAppDetails(45, 1'000, 2'000, true), "new game details");
    const auto* install = find_row(model, "details-install");
    check(install && install->subtitle.find("To M.2 SSD · ") == 0 && install->subtitle.find(" free") != std::string::npos &&
              install->subtitle.find("reservation") == std::string::npos, "Install names the default drive and its free space");
}

const z3cloudplay::homebrew::Notice* find_notice(const Simulation& model, const std::string& id) {
    const auto found = std::find_if(model.state().notices.begin(), model.state().notices.end(),
                                    [&id](const auto& notice) { return notice.id == id; });
    return found == model.state().notices.end() ? nullptr : &*found;
}

void live_install_confirmation_checks_space_and_unknowns() {
    using z3cloudplay::homebrew::JobState;
    using z3cloudplay::homebrew::InstallRequestKind;
    constexpr std::uint64_t margin = 64ULL * 1024 * 1024;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "New"), game(43, "Queued")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(42, 100, 600, true);
    model.UpdateAppDetails(43, 100, 700, true);
    model.UpdateVolumes({{"internal", "Console storage", true, margin + 2'000, margin + 599},
                         {"m2", "M.2 SSD", true, margin + 2'000, margin + 1'000}}, "internal");
    check(model.action("details:42"), "details open");

    // A game that does not fit the default drive opens the sheet, which explains and cannot confirm.
    activate(model, "details-install");
    check(model.view().surface == "install" && !find_row(model, "install-confirm")->enabled,
          "a game that does not fit opens the install sheet without a usable Install");
    check(model.view().subtitle.find("Not enough space") != std::string::npos, "the sheet says why");
    check(model.TakeInstallRequests().empty(), "nothing is queued while it does not fit");

    // Another drive is for this install only: the default stays.
    activate(model, "storage-picker");
    check(model.view().title == "Install to" && model.view().subtitle.find("For this game only") == 0,
          "the picker says the choice is for this game only");
    activate(model, "volume-m2");
    check(model.view().surface == "install" && find_row(model, "install-confirm")->enabled,
          "a drive with room enables Install");
    check(model.view().subtitle.find("To M.2 SSD (this game only)") == 0 &&
              model.view().subtitle.find("Other games still install to Console storage") != std::string::npos,
          "the sheet names the one-off drive and the unchanged default");
    check(model.TakeInstallRequests().empty() && model.state().default_volume_id == "internal",
          "choosing a drive for one install does not change the default");
    activate(model, "install-confirm");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Install && requests[0].game_id == 42 &&
              requests[0].volume_id == "m2", "the install goes to the chosen drive");

    // Reservations: the rest of each download's estimate, less what it already holds on the drive.
    model.UpdateInstalls({{43, JobState::Downloading, 90, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 700, "a download that has reported nothing reserves its estimate");
    model.UpdateInstalls({{43, JobState::Downloading, 90, 100, {}, "m2", {}, {}, 300}}, {});
    check(model.state().volumes[1].reserved_bytes == 400, "bytes a download already holds on the drive are not reserved twice");
    model.UpdateInstalls({{43, JobState::Downloading, 90, 100, {}, "m2", {}, {}, 900}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "a preallocated download reserves nothing more");
    model.UpdateInstalls({{43, JobState::Failed, 90, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "failed downloads release live reservations");
    model.UpdateInstalls({{43, JobState::Cancelled, 90, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "cancelled downloads hold no reservation");
    model.UpdateInstalls({{43, JobState::Downloaded, 100, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "completed downloads hold no reservation");
    model.UpdateInstalls({}, {});

    // A game that fits installs at once, to the default drive.
    model.UpdateVolumes({{"internal", "Console storage", true, margin + 2'000, margin + 1'000}}, "internal");
    activate(model, "details-install");
    requests = model.TakeInstallRequests();
    check(model.view().surface != "install" && requests.size() == 1 && requests[0].volume_id.empty(),
          "a game that fits installs without the sheet");

    // An unknown size installs at once when the drive has the margin; the downloader checks it.
    model.UpdateAppDetails(42, std::nullopt, std::nullopt, true);
    check(find_row(model, "details-install")->value == "Size unknown", "unknown size never displays zero");
    activate(model, "details-install");
    check(model.TakeInstallRequests().size() == 1, "unknown size installs with a pre-download check");
    model.UpdateVolumes({{"internal", "Console storage", true, margin + 2'000, 0}}, "internal");
    activate(model, "details-install");
    check(model.view().surface == "install" && !find_row(model, "install-confirm")->enabled,
          "unknown size still needs the known download safety margin");
    activate(model, "cancel");

    // A drive whose free space cannot be read blocks; a console that cannot read it at all does not.
    model.UpdateVolumes({{"internal", "Console storage", true, std::nullopt, std::nullopt}}, "internal");
    activate(model, "details-install");
    check(!find_row(model, "install-confirm")->enabled &&
              model.view().subtitle.find("can't be read") != std::string::npos,
          "unknown capacity is honestly unavailable and blocks confirmation");
    check(model.TakeInstallRequests().empty(), "unknown capacity sends no install request");
    activate(model, "cancel");
    model.SetFreeSpaceSupported(false);
    model.UpdateVolumes({{"internal", "Console storage", true, std::nullopt, std::nullopt}}, "internal");
    activate(model, "details-install");
    check(model.TakeInstallRequests().size() == 1, "a console that cannot read free space still installs, as before");
}

void live_install_notifications() {
    using z3cloudplay::homebrew::JobState;
    auto model = make_live();
    model.SetWallClock(1'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Finishing"), game(43, "Failing"), game(44, "Removed")}, false, {}, false, false, {}}),
          "library loaded");

    // The first snapshot is restored state: a failed job from before restart is not announced.
    model.UpdateInstalls({{42, JobState::Downloading, 10, 100, {}, "internal"}, {43, JobState::Failed, 0, 100, "Old failure", "internal"}},
                         {{44, 500, "internal", true}});
    check(model.state().notices.empty(), "restored history raises no notification");

    // A job that finishes into an install, and a job that newly fails.
    model.UpdateInstalls({{43, JobState::Downloading, 5, 100, {}, "internal"}}, {{42, 100, "internal", true}, {44, 500, "internal", true}});
    model.UpdateInstalls({{43, JobState::Failed, 5, 100, "Steam refused the download.", "internal"}},
                         {{42, 100, "internal", true}, {44, 500, "internal", true}});
    const auto* done = find_notice(model, "install-42");
    check(done && !done->persistent && !done->failure && done->message == "Download complete" && done->detail == "Finishing",
          "a finished download is announced with the game's name");
    const auto* failed = find_notice(model, "install-43");
    check(failed && failed->failure && !failed->persistent && failed->message == "Download failed" &&
              failed->detail == "Failing · Steam refused the download." && model.state().navigation.modals.empty(),
          "a new failure is announced with its reason in a corner, not a dialog");
    model.UpdateInstalls({{43, JobState::Failed, 5, 100, "Steam refused the download.", "internal"}},
                         {{42, 100, "internal", true}, {44, 500, "internal", true}});
    check(std::count_if(model.state().notices.begin(), model.state().notices.end(),
                        [](const auto& notice) { return notice.id == "install-43"; }) == 1 &&
              model.state().navigation.modals.empty(),
          "a failure that persists is not announced twice");

    // The More menu's Favorite says what it will do and closes the menu.
    check(model.action("details:44") && model.action("focus:details-more") && model.action("cross"), "menu opens");
    check(find_row(model, "favorite") && find_row(model, "favorite")->title == "Add to Favorites", "menu offers to add");
    activate(model, "favorite");
    check(model.state().navigation.modals.empty() && model.state().library.favorites.contains(44),
          "Favorite from the menu marks the game and closes the menu");
    check(model.action("focus:details-more") && model.action("cross") &&
              find_row(model, "favorite")->title == "Remove from Favorites", "menu offers to remove a favorite");
    activate(model, "favorite");
    check(!model.state().library.favorites.contains(44), "removing works the same way");

    // Uninstall: only a removal the player asked for is announced.
    check(model.action("details:44") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    activate(model, "confirm-destructive");
    model.TakeInstallRequests();
    model.UpdateInstalls({{43, JobState::Failed, 5, 100, "Steam refused the download.", "internal"}}, {{42, 100, "internal", true}});
    const auto* removed = find_notice(model, "install-44");
    check(removed && removed->message == "Game uninstalled" && removed->detail == "Removed", "an uninstall is announced");

    // Notifications count down only while visible and then disappear.
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross")
              && !model.state().navigation.modals.empty(), "a dialog opens");
    model.SetWallClock(1'030);
    check(find_notice(model, "install-42") != nullptr, "notifications wait behind a dialog");
    check(model.action("circle"), "the dialog closes");
    model.SetWallClock(1'033);
    check(find_notice(model, "install-42") != nullptr, "a visible notification lasts a few seconds");
    model.SetWallClock(1'040);
    check(!find_notice(model, "install-42") && !find_notice(model, "install-44"), "visible notifications expire");

    // Controller: a lasting warning while disconnected, a short notice on reconnect.
    model.SetControllerConnected(false);
    const auto* lost = find_notice(model, "controller");
    check(lost && lost->persistent && lost->failure, "a disconnected controller raises a lasting warning");
    model.SetControllerConnected(true);
    check(!find_notice(model, "controller") && find_notice(model, "controller-connected"),
          "reconnecting clears the warning and says so briefly");
}

void live_log_viewer() {
    using z3cloudplay::homebrew::InstallRequestKind;
    using z3cloudplay::homebrew::JobState;
    using z3cloudplay::homebrew::LogState;
    const auto directory = std::filesystem::temp_directory_path() / ("z3cloudplay-model-logs-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / "launch.log");  // unreadable as a file
    {
        std::ofstream log(directory / "install.log");
        for (int index = 0; index < 12; ++index)
            log << "2026-10-05T07:00:" << (10 + index) << "Z app=" << (index % 2 ? 43 : 42) << " step=trace result=r" << index << " detail=\n";
    }
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Fine"), game(43, "Broken")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({{43, JobState::Failed, 0, 100, "Download failed.", "internal"}}, {});

    // Without a log folder the Settings entry is unavailable.
    model.action("l1");
    const auto* logs_row = find_row(model, "settings-logs");
    check(logs_row && !logs_row->enabled, "no log folder: Logs is unavailable");
    model.SetLogDirectory(directory);
    logs_row = find_row(model, "settings-logs");
    check(logs_row && logs_row->enabled, "with a log folder: Logs opens");

    activate(model, "settings-logs");
    check(has_row(model, "log-install") && has_row(model, "log-auth") && has_row(model, "log-launch"), "the log picker lists each log");
    activate(model, "log-install");
    check(model.state().log_state == LogState::Ready && model.state().log_lines.size() == 12, "the install log loads every line");
    check(model.state().log_offset == 4 && has_row(model, "log-line-11") && !has_row(model, "log-line-3"),
          "the log opens at its newest lines");
    check(model.state().log_context.find("12 lines") != std::string::npos, "the log says how many lines it shows");
    check(has_row(model, "logs-refresh") && !has_row(model, "job-retry-43"), "a general log offers Refresh, not Retry");
    activate(model, "logs-back");

    activate(model, "log-auth");
    check(model.state().log_state == LogState::Ready && model.state().log_lines.empty() && has_row(model, "logs-empty"),
          "a log that does not exist yet is empty, not an error");
    activate(model, "logs-back");
    activate(model, "log-launch");
    check(model.state().log_state == LogState::Failed && has_row(model, "logs-error"), "an unreadable log reports a failure");
    activate(model, "logs-back");
    model.action("circle");

    // From a failed download: that game's lines only, with Retry.
    model.action("l1");
    model.action("l1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open");
    activate(model, "job-43");
    activate(model, "logs");
    check(model.state().log_lines.size() == 6, "a game's log keeps only that game's lines");
    check(std::all_of(model.state().log_lines.begin(), model.state().log_lines.end(),
                      [](const std::string& line) { return line.find("app=43 ") != std::string::npos; }), "only app 43 lines");
    check(model.state().log_context.rfind("Broken · ", 0) == 0, "the game's log names the game");
    check(has_row(model, "job-retry-43"), "a failed download's log offers Retry");
    {
        std::ofstream log(directory / "install.log", std::ios::app);
        log << "2026-10-05T07:01:00Z app=43 step=install result=network detail=\n";
    }
    activate(model, "logs-refresh");
    check(model.state().log_lines.size() == 7, "Refresh reads new lines");
    activate(model, "job-retry-43");
    const auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Resume && requests[0].game_id == 43,
          "Retry from the log resumes the download");
    check(model.state().navigation.modals.empty(), "Retry closes the log and the job's actions");

    // Resume all puts every paused download back in the queue.
    model.UpdateInstalls({{43, JobState::Paused, 10, 100, {}}, {44, JobState::Paused, 0, 0, {}},
                          {45, JobState::Downloading, 5, 50, {}}}, {});
    activate(model, "job-45");
    check(has_row(model, "job-resume-all"), "a running download's actions offer Resume all while others wait");
    activate(model, "job-resume-all");
    const auto resumed = model.TakeInstallRequests();
    check(resumed.size() == 2 && resumed[0].kind == InstallRequestKind::Resume && resumed[0].game_id == 43 &&
              resumed[1].kind == InstallRequestKind::Resume && resumed[1].game_id == 44,
          "Resume all resumes each paused download");
    check(model.state().navigation.modals.empty(), "Resume all closes the actions");
    model.UpdateInstalls({{43, JobState::Paused, 10, 100, {}}, {45, JobState::Downloading, 5, 50, {}}}, {});
    activate(model, "job-43");
    check(!has_row(model, "job-resume-all"), "a single paused download offers only its own Resume");
    model.action("circle");
    std::filesystem::remove_all(directory);
}

void live_cloud_saves() {
    using z3cloudplay::homebrew::CloudRequestKind;
    using z3cloudplay::homebrew::CloudStatus;
    auto model = make_live();
    model.SetWallClock(10 * 86'400);
    model.UpdateAccount({AuthState::SignedIn, "123", "Player", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Saved Game"), game(43, "Not Installed")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{42, 100, "internal", true}});

    // A runtime that can't start is said on every screen, with the reason in Settings (#64).
    model.UpdateRuntimeProblem("managed runtime data root is inaccessible");
    check(std::any_of(model.state().notices.begin(), model.state().notices.end(),
                      [](const auto& notice) { return notice.id == "runtime" && notice.persistent; }),
          "a runtime that can't start shows a lasting notice");
    check(model.state().runtime_problem.find("/data/z3cloudplay") != std::string::npos, "the reason says what to fix");
    model.UpdateRuntimeProblem("");
    check(model.state().runtime_problem.empty() &&
              std::none_of(model.state().notices.begin(), model.state().notices.end(),
                           [](const auto& notice) { return notice.id == "runtime"; }),
          "the notice goes once the runtime starts");

    // Settings turns automatic Cloud saves on and off; it is off by default (a fresh install never
    // changes Cloud saves unasked, #69) and saved with the preferences.
    check(!model.state().cloud_saves_auto && !model.ExportPreferences().cloud_saves_auto, "automatic Cloud saves start off");
    model.action("l1");
    const auto* automatic = find_row(model, "settings-cloud-saves");
    check(automatic && !automatic->selected, "Settings shows automatic Cloud saves off");
    activate(model, "settings-cloud-saves");
    check(model.state().cloud_saves_auto && model.ExportPreferences().cloud_saves_auto, "the toggle turns it on");
    activate(model, "settings-cloud-saves");
    check(!model.state().cloud_saves_auto, "and off again");
    activate(model, "settings-cloud-saves");
    check(model.state().cloud_saves_auto, "and on for what follows");

    check(model.action("details:43") && model.UpdateAppDetails(43, 1, 2, true) && !has_row(model, "details-cloud"),
          "a game that is not installed has no Cloud saves row");
    check(model.action("details:42"), "installed game opens");
    const auto* saves = find_row(model, "details-cloud");
    check(saves && saves->enabled && saves->action == "cloud-sync" && saves->value == "Sync", "an installed game offers Cloud saves");
    activate(model, "details-cloud");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::Sync && requests[0].game_id == 42, "Cloud saves asks for a sync");

    model.UpdateCloud({{42, CloudStatus::Checking, "Checking Steam Cloud…"}});
    saves = find_row(model, "details-cloud");
    check(saves && !saves->enabled && saves->value == "Checking…", "a running sync cannot be started twice");
    check(saves->kind == z3cloudplay::homebrew::RowKind::Action, "Cloud saves stays a button while it works");
    model.UpdateCloud({{42, CloudStatus::Synced, "2 saves downloaded"}});
    saves = find_row(model, "details-cloud");
    check(saves && saves->enabled && saves->value == "Synced" && saves->subtitle == "2 saves downloaded", "a finished sync says what moved");
    const auto* synced = find_notice(model, "cloud-42");
    check(synced && synced->message == "Cloud saves synced" && synced->detail == "Saved Game · 2 saves downloaded", "a sync is announced");

    // A conflict opens a dialog describing both sides; the choice goes to the service.
    model.UpdateCloud({{42, CloudStatus::Conflict, "Saves here and in Steam Cloud both changed.", 2, 3, 2'048, 4'096,
                        9 * 86'400, 10 * 86'400}});
    check(find_notice(model, "cloud-42")->message == "Cloud saves need a choice", "a pending choice is announced");
    saves = find_row(model, "details-cloud");
    check(saves && saves->action == "cloud-choose" && saves->value == "Choose", "a conflict asks for a choice");
    activate(model, "details-cloud");
    const auto& modal = model.state().navigation.modals.back();
    check(modal.id == "confirmation" && modal.body.find("Steam Cloud: 3 files") != std::string::npos
              && modal.body.find("This console: 2 files") != std::string::npos
              && modal.body.find("changed yesterday") != std::string::npos,
          "the choice dialog describes both sides");
    check(has_row(model, "cloud-use-cloud") && has_row(model, "cloud-use-local"), "both choices are offered");
    activate(model, "cancel");
    model.UpdateCloud({{42, CloudStatus::Conflict, "Saves here and in Steam Cloud both changed.", 0, 1, 0, 4'096, 0, 10 * 86'400}});
    activate(model, "details-cloud");
    const auto& empty_side = model.state().navigation.modals.back().body;
    check(empty_side.find("Steam Cloud: 1 file, ") != std::string::npos && empty_side.find("This console: no saves.") != std::string::npos,
          "a side without saves says so, and one file is not \"1 files\"");
    activate(model, "cancel");
    model.UpdateCloud({{42, CloudStatus::Conflict, "Saves here and in Steam Cloud both changed.", 2, 3, 2'048, 4'096,
                        9 * 86'400, 10 * 86'400}});
    activate(model, "details-cloud");
    activate(model, "cloud-use-local");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::UseLocal && model.state().navigation.modals.empty(),
          "choosing local saves goes to the service and closes the dialog");

    // Uploads for an account Z3CloudPlay has not synced before need its confirmation.
    model.UpdateCloud({{42, CloudStatus::NeedsAccount, "Confirm this Steam account."}});
    activate(model, "details-cloud");
    check(model.state().navigation.modals.back().body.find("Player") != std::string::npos && has_row(model, "cloud-confirm-account"),
          "the account dialog names the signed-in account");
    activate(model, "cloud-confirm-account");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::ConfirmAccount, "confirming goes to the service");

    model.UpdateCloud({{42, CloudStatus::Failed, "Steam is not reachable. Try again."}});
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Cloud sync failed",
          "a sync failure waits in a dialog");
    activate(model, "alert-ok");
    check(find_row(model, "details-cloud")->value == "Retry", "a failure can be retried");
}

void refresh_actions_preserve_authentication_and_wall_clock_is_external() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(1, "One")}, false, {}, false, true, 100}),
          "baseline for refresh action accepted");
    model.action("l1");
    activate(model, "settings-account");
    activate(model, "account-sync");
    const auto actions = model.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::RefreshLibrary
              && model.state().account.authentication == AuthState::SignedIn
              && model.state().library_syncing && model.state().library_loading,
          "visible refresh action starts library delivery without changing account authentication");
    check(model.UpdateLibrary({{}, true, {}, false, true, std::nullopt})
              && model.state().account.authentication == AuthState::SignedIn,
          "loading update retains authenticated account state");
    check(model.UpdateLibrary({{game(1, "One refreshed")}, false, {}, false, true, 101})
              && model.state().account.authentication == AuthState::SignedIn
              && !model.state().library_syncing && !model.state().library_loading,
          "terminal library update clears delivery status without restoring authentication");

    auto offline = make_live();
    offline.UpdateAccount({AuthState::Offline, "account", "Owner", {}, {}, false});
    check(offline.action("refresh-library")
              && offline.state().account.authentication == AuthState::Offline
              && offline.state().library_syncing && offline.state().library_loading,
          "direct refresh preserves offline authentication state");

    check(model.state().clock_ticks == 0, "live model starts without invented wall time");
    model.SetWallClock(4'102'444'900LL);
    check(model.state().clock_ticks == 4'102'444'900LL,
          "controller can inject current Unix time into live presentation");
    model.SetWallClock(4'102'444'900LL);
    check(model.state().clock_ticks == 4'102'444'900LL,
          "repeating wall time is an idempotent update");

    Simulation simulation("fixtures-not-used", "profile-not-used.json", ModelMode::Simulation);
    const auto fixture_clock = simulation.state().clock_ticks;
    simulation.SetWallClock(1);
    check(simulation.state().clock_ticks == fixture_clock,
          "external wall clock cannot alter deterministic simulation time");
}

void live_filters_only_use_available_facts() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(42,"Synthetic")},false,{},false,false,{}}),"library loaded");
    check(model.state().games.front().folded_title=="synthetic","production delivery caches Unicode key once");
    check(model.action("r1") && model.action("triangle"),"live filter modal opens");
    const auto& actions=model.state().navigation.modals.back().action_keys;
    for(const auto& unavailable:{"sort-recent-added","sort-install-size","filter-installed","filter-genre-adventure","filter-controller-full"})
        check(std::find(actions.begin(),actions.end(),unavailable)==actions.end(),"unknown facts are not filter choices");
    check(std::find(actions.begin(),actions.end(),"filter-compat-playable")!=actions.end(),"personal compatibility remains filterable");

    // A CM acquisition date makes "Recently added" a real fact, sorted newest first.
    auto dated=make_live();
    dated.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    auto older=game(42,"Alpha"),newer=game(43,"Beta"),undated=game(44,"Gamma");
    older.added_at_unix_seconds=1400000000;newer.added_at_unix_seconds=1500000000;
    check(dated.UpdateLibrary({{older,newer,undated},false,{},false,false,{}}),"dated library loaded");
    check(dated.state().games.front().added_at==1400000000,"acquisition date projected");
    check(dated.action("r1") && dated.action("triangle"),"dated filter modal opens");
    const auto& dated_actions=dated.state().navigation.modals.back().action_keys;
    check(std::find(dated_actions.begin(),dated_actions.end(),"sort-recent-added")!=dated_actions.end(),"recently added offered when dates exist");
    activate(dated,"sort-recent-added");
    check(dated.state().library.sort=="recent-added","recently added sort selected");
    const auto ordered=dated.visible_games();
    check(ordered.size()==3 && ordered[0]->id==43 && ordered[1]->id==42 && ordered[2]->id==44,
          "newest acquisition first, undated last");
}

void live_uninstall_removes_title_and_keeps_saves() {
    using z3cloudplay::homebrew::InstallRequestKind;
    using z3cloudplay::homebrew::LiveRuntimeGame;
    using z3cloudplay::homebrew::RowKind;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Synthetic"), game(43, "No title")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(42, 300, 600, true);
    model.UpdateInstalls({}, {{42, 600'000'000}, {43, 100}});
    LiveRuntimeGame ready;
    ready.game_id = 42; ready.status = "Ready"; ready.ready = true; ready.can_play = true; ready.has_title = true;
    LiveRuntimeGame plain;
    plain.game_id = 43; plain.status = "Not prepared"; plain.can_prepare = true;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {ready, plain});

    // A prepared game: the dialog says the PS5 title goes too and offers deleting saves, off by default.
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    const auto& modal = model.state().navigation.modals.back();
    check(modal.body.find("PS5 title are removed") != std::string::npos, "the dialog names the PS5 title");
    check(modal.body.find("Steam Cloud saves are synced first") != std::string::npos, "Cloud is synced first");
    const auto* toggle = find_row(model, "uninstall-delete-saves");
    check(toggle && toggle->kind == RowKind::Toggle && !toggle->selected, "deleting saves is a choice, off by default");
    activate(model, "uninstall-delete-saves");
    toggle = find_row(model, "uninstall-delete-saves");
    check(toggle && toggle->selected && !model.state().navigation.modals.empty(), "the choice toggles in the open dialog");
    check(model.state().navigation.modals.back().body.find("saves and settings on this console are deleted") != std::string::npos &&
          model.state().navigation.modals.back().body.find("are kept") == std::string::npos,
          "the dialog says the saves go when the choice is on");
    activate(model, "confirm-destructive");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Uninstall && requests[0].game_id == 42 &&
              requests[0].delete_saves, "Uninstall carries the saves choice");

    // While it runs, Play waits; an interrupted removal offers Retry on the PS5 title row.
    ready.uninstall_status = "Removing PS5 title…"; ready.can_play = false;
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {ready, plain});
    check(model.action("details:42"), "details reopen");
    const auto* play = find_row(model, "details-play");
    check(play && !play->enabled && play->value == "Removing PS5 title…", "Play shows the uninstall in progress");
    LiveRuntimeGame stuck;
    stuck.game_id = 42; stuck.status = "Removal incomplete"; stuck.can_prepare = true; stuck.has_title = true;
    stuck.detail = "The console has not confirmed that the PS5 title is gone. Retry checks again.";
    model.UpdateRuntime(z3cloudplay::homebrew::RuntimeState::Ready, {stuck, plain});
    check(model.action("details:42"), "details reopen");
    const auto* retry = find_row(model, "details-play");
    check(retry && retry->title == "Retry" && retry->enabled && retry->action == "runtime-remove-retry",
          "an unfinished removal turns Play into Retry");
    activate(model, "details-play");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Uninstall && !requests[0].delete_saves,
          "Retry continues the uninstall with the saved choice");
    model.NotifyUninstallStopped(42, "The PS5 title could not be removed.");
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Uninstall stopped" &&
              model.state().navigation.modals.back().action_keys == std::vector<std::string>{"alert-ok"},
          "a stopped uninstall says why in a dialog with OK");
    activate(model, "alert-ok");
    check(model.state().navigation.modals.empty(), "OK dismisses the failure dialog");
    model.NotifyPlayStopped(42, "This game's program has Steam DRM that Z3CloudPlay can't unpack on the PS5 yet.");
    model.NotifyPlayStopped(42, "This game's program has Steam DRM that Z3CloudPlay can't unpack on the PS5 yet.");
    check(model.state().navigation.modals.size() == 1 && model.state().navigation.modals.back().title == "Can't play" &&
              model.state().navigation.modals.back().body.find("Steam DRM") != std::string::npos,
          "a refused Play says why, once however often it is pressed");
    activate(model, "alert-ok");
    check(model.state().navigation.modals.empty(), "a repeated refusal is not asked twice");

    // A game without a PS5 title and without Cloud: no saves choice; its saves exist only here.
    model.UpdateCloud({{43, z3cloudplay::homebrew::CloudStatus::Unsupported, "No Steam Cloud saves"}});
    check(model.action("details:43") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    check(!has_row(model, "uninstall-delete-saves"), "no saves choice without a PS5 title");
    check(model.state().navigation.modals.back().body.find("exist only on this console") != std::string::npos,
          "without Cloud the dialog says the saves exist only here");
    activate(model, "cancel");
    while (!model.state().navigation.modals.empty()) model.action("circle");

    // An uninstalled game with kept saves: Details shows them and can delete them.
    model.UpdateInstalls({}, {{43, 100}});
    model.UpdateKeptBottles({{42, "Synthetic", 1760000000, 5'000'000}});
    check(model.action("details:42"), "details open");
    const auto* kept = find_row(model, "details-kept");
    check(kept && kept->title == "Saves from a previous install" && !kept->value.empty(), "kept saves are shown with their size");
    check(model.state().navigation.focus_key != "details-kept-delete", "Delete saves is never focused by default");
    {
        // While Steam is still checked there is no Install yet: focus still avoids Delete saves.
        model.UpdateInstalls({}, {});
        model.UpdateKeptBottles({{42, "Synthetic", 1760000000, 5'000'000}, {43, "No title", 1760000000, 1'000'000}});
        check(model.action("details:43") && has_row(model, "details-checking") && has_row(model, "details-kept-delete"),
              "kept saves while Steam is checked");
        check(model.state().navigation.focus_key != "details-kept-delete", "a second Cross cannot land on Delete saves");
        model.UpdateInstalls({}, {{43, 100}});
        model.UpdateKeptBottles({{42, "Synthetic", 1760000000, 5'000'000}});
        check(model.action("details:42"), "details reopen");
    }
    check(has_row(model, "details-install"), "Install brings them back");
    activate(model, "details-kept-delete");
    check(model.state().navigation.modals.back().title == "Delete kept saves?", "deleting asks first");
    activate(model, "confirm-destructive");
    auto deletes = model.TakeKeptDeleteRequests();
    check(deletes.size() == 1 && deletes[0] == 42, "the confirmed deletion goes to the runtime");

    // Settings shows kept game data as one row that opens the manager.
    model.UpdateKeptBottles({{42, "Synthetic", 1760000000, 5'000'000}, {43, "", 1760000000, 3'000'000},
                             {44, "Third", 0, 1'000'000}});
    model.action("l1");
    check(model.state().navigation.route == z3cloudplay::homebrew::Route::Settings, "Settings opens");
    check(!has_row(model, "settings-kept-42"), "kept games are not listed one by one in Settings");
    const auto* entry = find_row(model, "settings-kept");
    check(entry && entry->title == "Kept game data" && entry->value.find("3 games · ") == 0, "the row counts kept games and size");
    activate(model, "settings-kept");
    check(model.state().navigation.modals.back().id == "kept-data", "the row opens the kept game data manager");
    const auto* named = find_row(model, "kept-toggle-43");
    check(named && named->title == "No title" && named->kind == RowKind::Toggle && !named->selected,
          "a game without a kept name uses the library's, unchecked");
    check(find_row(model, "kept-toggle-42")->subtitle == "Kept 2025-10-09", "each game shows when it was kept");
    check(!find_row(model, "kept-selection-delete")->enabled, "nothing to delete until a game is checked");
    activate(model, "kept-toggle-43");
    check(find_row(model, "kept-toggle-43")->selected && !model.state().navigation.modals.empty(), "checking stays in the manager");
    check(find_row(model, "kept-selection-delete")->value.find("1 game · ") == 0, "Delete names how many");
    activate(model, "kept-select-all");
    check(find_row(model, "kept-toggle-42")->selected && find_row(model, "kept-toggle-44")->selected, "Select all checks every game");
    check(find_row(model, "kept-select-all")->title == "Clear selection", "with all checked it clears instead");
    activate(model, "kept-select-all");
    check(!find_row(model, "kept-toggle-42")->selected, "Clear selection unchecks every game");
    activate(model, "kept-toggle-42");
    activate(model, "kept-toggle-44");
    activate(model, "kept-selection-delete");
    const auto& confirm = model.state().navigation.modals.back();
    check(confirm.title == "Delete kept game data?" && confirm.body.find("2 games (6 MB)") != std::string::npos,
          "the confirmation names how many games and how much space");
    activate(model, "cancel");
    check(model.TakeKeptDeleteRequests().empty() && model.state().navigation.modals.back().id == "kept-data",
          "cancelling deletes nothing and returns to the manager");
    activate(model, "kept-selection-delete");
    activate(model, "confirm-destructive");
    deletes = model.TakeKeptDeleteRequests();
    check(deletes == std::vector<z3cloudplay::homebrew::GameId>{42, 44}, "the confirmed games go to the runtime");
    check(model.state().navigation.modals.empty() && model.state().navigation.focus_key == "settings-kept",
          "the manager closes and focus returns to its row");

    // Kept data changes while the manager is open: it lists what is left, and closes when nothing is.
    activate(model, "settings-kept");
    model.UpdateKeptBottles({{43, "", 1760000000, 3'000'000}});
    check(!has_row(model, "kept-toggle-42") && has_row(model, "kept-toggle-43"), "the open manager follows the kept list");
    model.UpdateKeptBottles({});
    check(model.state().navigation.modals.empty(), "the manager closes when nothing is kept");
    entry = find_row(model, "settings-kept");
    check(entry && entry->value == "None" && !entry->enabled, "nothing kept says so");
    check(model.state().navigation.focus_key == "settings-kept", "focus stays on the row");
}

void live_settings_focus_stays_near_a_removed_row() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.SetRuntimeLeftovers(2);
    model.action("l1");
    activate(model, "settings-runtime-cleanup");
    check(!model.TakeRuntimeCleanupRequest() && !model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().destructive, "cleanup asks before deleting");
    activate(model, "cancel");
    check(!model.TakeRuntimeCleanupRequest() && has_row(model, "settings-runtime-cleanup"), "Cancel keeps the data");
    activate(model, "settings-runtime-cleanup");
    activate(model, "runtime-cleanup-confirm");
    check(model.TakeRuntimeCleanupRequest(), "cleanup goes to the runtime");
    model.SetRuntimeLeftovers(0);
    const auto& rows = model.view().rows;
    const auto focused = rows[static_cast<std::size_t>(model.view().focused_row)];
    check(!has_row(model, "settings-runtime-cleanup"), "the cleaned row is gone");
    check(focused.section == "Library" && !focused.rail_only && !model.state().navigation.settings_rail_focused,
          "focus moves to the nearest row in the same section, not the rail");
    check(focused.enabled, "a row that can be chosen is preferred");
}

void preparation_steps_read_as_player_words() {
    using z3cloudplay::homebrew::PreparationStepWords;
    for (const char* step : {"stage-title", "copy-template", "validate-artifacts", "publish-profile", "query-native",
                             "register", "await-native", "capture-settings", "something-new"}) {
        const auto words = PreparationStepWords(step);
        check(!words.empty() && words.find('-') == std::string::npos, "every step reads as words, never its identifier");
    }
    check(PreparationStepWords("copy-template") == "Copying the Windows environment", "the template copy is named");
    check(PreparationStepWords("await-native") == "Waiting for the PS5", "registration waits for the PS5");
    check(PreparationStepWords("").empty(), "no step, no words");
    using z3cloudplay::homebrew::PreparationProgressWords;
    check(PreparationProgressWords(false, "") == "Step 1 of 6 · Reserving a PS5 title", "before its record, the title is reserved");
    check(PreparationProgressWords(true, "copy-template") == "Step 3 of 6 · Copying the Windows environment",
          "the template copy is step 3");
    check(PreparationProgressWords(true, "await-native") == "Step 6 of 6 · Waiting for the PS5", "registration is the last step");
    check(PreparationProgressWords(true, "something-new") == "Preparing", "an unknown step has no number");
}

void live_details_focus_moves_to_play_once_enabled() {
    using z3cloudplay::homebrew::LiveRuntimeGame;
    using z3cloudplay::homebrew::RuntimeState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(42, 300, 600, true);
    model.UpdateInstalls({}, {{42, 600'000'000}});
    check(model.action("details:42"), "details open");
    // The runtime is still checking: no Play yet, focus waits on the first enabled action.
    check(model.state().navigation.focus_key != "details-play", "no Play while the runtime checks");
    LiveRuntimeGame checking; checking.game_id = 42; checking.status = "Checking";
    model.UpdateRuntime(RuntimeState::Checking, {checking});
    check(model.state().navigation.focus_key == "details-cloud", "a disabled Play leaves focus on Cloud saves for now");
    LiveRuntimeGame ready; ready.game_id = 42; ready.status = "Ready"; ready.ready = true; ready.can_play = true; ready.has_title = true;
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.focus_key == "details-play", "focus moves to Play once it is enabled");

    // Once the player moves, Play becoming enabled leaves focus where they put it.
    model.UpdateRuntime(RuntimeState::Checking, {checking});
    check(model.action("details:42"), "details reopen");
    check(model.action("right"), "the player moves");
    const auto chosen = model.state().navigation.focus_key;
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.focus_key == chosen, "focus stays where the player moved it");

    // Left and Right step between the drawn buttons only: the install line and the PS5 title note are not stops.
    check(model.action("focus:details-play"), "focus Play");
    check(model.action("left") && model.state().navigation.focus_key == "details-play", "Left from Play stays on Play");
    std::vector<std::string> visited{model.state().navigation.focus_key};
    for (int step = 0; step < 6; ++step) {
        check(model.action("right"), "move right");
        if (model.state().navigation.focus_key != visited.back()) visited.push_back(model.state().navigation.focus_key);
    }
    for (const auto& key : visited) {
        const auto* item = find_row(model, key);
        check(item && (item->kind == z3cloudplay::homebrew::RowKind::Action || item->kind == z3cloudplay::homebrew::RowKind::Toggle),
              "every stop is a button");
    }
    check(std::find(visited.begin(), visited.end(), "details-runtime") == visited.end() &&
              std::find(visited.begin(), visited.end(), "details-installed") == visited.end(),
          "notes are skipped");
}

void live_ready_to_play_quits_to_home() {
    using z3cloudplay::homebrew::LiveRuntimeGame;
    using z3cloudplay::homebrew::RuntimeState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Synthetic"), game(43, "Other")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(42, 300, 600, true);
    model.UpdateInstalls({}, {{42, 600'000'000}, {43, 100}});
    LiveRuntimeGame waiting; waiting.game_id = 42; waiting.status = "Waiting for the PS5"; waiting.awaiting_registration = true;
    waiting.can_prepare = true; waiting.has_title = true;
    waiting.detail = "Waiting for the PS5, which adds the game to Home when Z3CloudPlay quits";
    model.UpdateRuntime(RuntimeState::Ready, {waiting});

    // Details: Play is ready and quits to Home without asking; the PS5 title row checks again.
    check(model.action("details:42"), "details open");
    const auto* play = find_row(model, "details-play");
    check(play && play->enabled && play->action == "quit-to-home" && play->value == "Ready to play", "Play reads Ready to play");
    check(find_row(model, "details-runtime")->value == "Waiting for the PS5", "the note says the title waits for the PS5");
    check(model.state().navigation.focus_key == "details-play", "focus rests on Play");
    // Its saves sync first (#50): Play waits in a dialog, then quits once they are in step.
    model.UpdateCloud({{42, CloudStatus::UpToDate, "Saves are up to date"}});
    activate(model, "details-play");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::Sync && requests[0].game_id == 42, "Play syncs saves first");
    check(!model.state().quit_requested && model.state().navigation.modals.back().title == "Syncing Cloud saves…",
          "Play waits for the sync in a dialog");
    model.UpdateCloud({{42, CloudStatus::UpToDate, "Saves are up to date"}});
    check(!model.state().quit_requested, "a result that is not the sync's answer does not count");
    model.NotifyCloudAnswered(43, CloudStatus::Synced);
    check(!model.state().quit_requested, "another game's answer does not count");
    model.UpdateCloud({{42, CloudStatus::Synced, "1 save downloaded"}});
    model.NotifyCloudAnswered(42, CloudStatus::Synced);
    check(model.state().quit_requested && model.state().navigation.modals.empty(), "Play quits to Home once saves are in step");
    // A choice or a failure lets the player play without syncing.
    auto choosing = make_live();
    choosing.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(choosing.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    choosing.UpdateInstalls({}, {{42, 600'000'000}});
    choosing.UpdateRuntime(RuntimeState::Ready, {waiting});
    check(choosing.action("details:42"), "details open");
    activate(choosing, "details-play");
    choosing.UpdateCloud({{42, CloudStatus::Conflict, "Both changed."}});
    choosing.NotifyCloudAnswered(42, CloudStatus::Conflict);
    check(!choosing.state().quit_requested && choosing.state().navigation.modals.back().title == "Cloud saves need a choice" &&
              has_row(choosing, "saves-quit-anyway"),
          "a conflict asks before quitting");
    activate(choosing, "saves-quit-anyway");
    check(choosing.state().quit_requested, "Play without syncing quits");
    // The console refusing the sync (the title may run) does not leave Play waiting.
    auto refused = make_live();
    refused.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(refused.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    refused.UpdateInstalls({}, {{42, 600'000'000}});
    refused.UpdateRuntime(RuntimeState::Ready, {waiting});
    check(refused.action("details:42"), "details open");
    activate(refused, "details-play");
    refused.NotifyCloudRefused(42);
    check(!refused.state().quit_requested && refused.state().navigation.modals.back().title == "The game may still be running" &&
              has_row(refused, "saves-quit-anyway"),
          "a refused sync says why and offers Play without syncing");
    // A game without Steam Cloud quits at once.
    auto local = make_live();
    local.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(local.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    local.UpdateInstalls({}, {{42, 600'000'000}});
    local.UpdateRuntime(RuntimeState::Ready, {waiting});
    local.UpdateCloud({{42, CloudStatus::Unsupported, "This game does not use Steam Cloud."}});
    check(local.action("details:42"), "details open");
    activate(local, "details-play");
    check(local.state().quit_requested && local.state().navigation.modals.empty(), "Play quits to Home without a dialog");

    // The prompt: shown when nothing else is open, once per game, Play quits, Later closes.
    auto prompted = make_live();
    prompted.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(prompted.UpdateLibrary({{game(42, "Synthetic"), game(43, "Other")}, false, {}, false, false, {}}), "library loaded");
    prompted.UpdateInstalls({}, {{42, 600'000'000}, {43, 100}});
    LiveRuntimeGame other = waiting; other.game_id = 43;
    prompted.UpdateRuntime(RuntimeState::Ready, {waiting, other});
    prompted.action("l1");
    activate(prompted, "settings-about");  // a sheet is open: the prompt waits
    const auto open_before = prompted.state().navigation.modals.size();
    prompted.NotifyReadyToPlay(42);
    prompted.NotifyReadyToPlay(42);
    check(prompted.state().navigation.modals.size() == open_before, "the prompt waits for the open sheet");
    check(prompted.action("circle"), "the sheet closes");
    check(!prompted.state().navigation.modals.empty(), "then the prompt shows");
    const auto& prompt = prompted.state().navigation.modals.back();
    check(prompt.title == "Synthetic is ready to play" && prompt.action_keys[static_cast<std::size_t>(prompt.focused_action)] == "quit-to-home",
          "the prompt names the game and focuses Play");
    check(find_row(prompted, "quit-to-home")->title == "Play" && find_row(prompted, "ready-later")->title == "Later", "Play or Later");
    activate(prompted, "ready-later");
    check(prompted.state().navigation.modals.empty() && !prompted.state().quit_requested, "Later closes it, once per game");
    // A game that stopped waiting (the check found it registered) is not announced.
    prompted.NotifyReadyToPlay(43);
    check(!prompted.state().navigation.modals.empty(), "another game is announced");
    activate(prompted, "quit-to-home");
    requests = prompted.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].game_id == 43, "Play in the prompt syncs the announced game");
    prompted.NotifyCloudAnswered(43, CloudStatus::Synced);
    check(prompted.state().quit_requested, "Play in the prompt quits to Home");
    auto dropped = make_live();
    dropped.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(dropped.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    dropped.UpdateInstalls({}, {{42, 600'000'000}});
    LiveRuntimeGame ready; ready.game_id = 42; ready.status = "Ready"; ready.ready = true; ready.can_play = true;
    dropped.UpdateRuntime(RuntimeState::Ready, {ready});
    dropped.NotifyReadyToPlay(42);
    check(dropped.state().navigation.modals.empty(), "a Ready game needs no prompt");
}

void live_play_does_the_next_useful_thing() {
    using z3cloudplay::homebrew::RuntimeState;
    using z3cloudplay::homebrew::LiveRuntimeGame;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{42, 600'000'000}});
    const auto show = [&](const std::string& status, bool ready, bool can_prepare) {
        LiveRuntimeGame value; value.game_id = 42; value.status = status; value.detail = status + " detail";
        value.ready = ready; value.can_play = ready; value.can_prepare = can_prepare; value.has_title = true;
        model.UpdateRuntime(RuntimeState::Ready, {value});
        check(model.action("details:42"), "details open");
        return find_row(model, "details-play");
    };
    // An update or a repair happens on the way to the game: Play stays Play and says so.
    for (const std::string status : {"Update available", "Needs repair"}) {
        const auto* play = show(status, false, true);
        check(play && play->title == "Play" && play->enabled && play->action == "runtime-play" &&
                  play->subtitle == status + " detail",
              "Play updates or repairs first, saying so");
        check(find_row(model, "details-runtime")->value ==
                  (status == "Update available" ? "Runtime update available" : "Runtime needs repair"),
              "the note names the runtime, not the game");
        activate(model, "details-play");
        check(model.TakeRuntimePlayRequests() == std::vector<z3cloudplay::GameId>{42}, "Play goes to the controller");
        // Only an update can happen without starting the game; a broken title is repaired by Play.
        const auto* update = find_row(model, "details-update-runtime");
        if (status == "Update available") {
            check(update && update->title == "Update runtime" && update->enabled && update->action == "runtime-update",
                  "Update runtime sits beside Play");
            activate(model, "details-update-runtime");
            check(model.TakeRuntimePrepareRequests() == std::vector<z3cloudplay::GameId>{42} &&
                      model.TakeRuntimePlayRequests().empty(),
                  "Update runtime updates without starting the game");
            check(model.state().navigation.focus_key == "details-play", "focus moves to Play, which shows the progress");
        } else {
            check(!update, "a repair offers no separate button");
        }
    }
    // While the runtime cannot act on it (queued, being played, uninstalling), there is no Update button.
    show("Update available", false, false);
    check(!find_row(model, "details-update-runtime"), "no Update runtime while the game cannot be prepared");
    // A title that needs the player's attention turns Play into that action.
    for (const auto& [status, label] : std::vector<std::pair<std::string, std::string>>{
             {"Needs recovery", "Resume"}, {"Failed", "Retry"}, {"Registration pending", "Check again"}}) {
        const auto* play = show(status, false, true);
        check(play && play->title == label && play->enabled && play->action == "runtime-prepare" &&
                  play->subtitle == status + " detail",
              "Play becomes the attention action");
        activate(model, "details-play");
        check(model.TakeRuntimePrepareRequests() == std::vector<z3cloudplay::GameId>{42}, "the action goes to the runtime host");
    }
    // Ready plays; while the runtime cannot act, Play is off and the note says why.
    check(show("Ready", true, false)->title == "Play" && find_row(model, "details-play")->enabled, "Ready plays");
    const auto* off = show("Preparing", false, false);
    check(off && off->title == "Play" && !off->enabled, "nothing to do leaves Play off");
    check(find_row(model, "details-runtime")->kind == z3cloudplay::homebrew::RowKind::Status, "the PS5 title is a note");
}

void live_store_details_fill_the_description() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateStoreDetails(42, "A synthetic game.", {"Action", "Indie"}, "full");
    const auto& games = model.state().games;
    check(games[0].description == "A synthetic game." && games[0].genres.size() == 2 && games[0].controller_support == "full",
          "the store fills the description, genres and controller support");
    check(model.UpdateLibrary({{game(42, "Synthetic")}, false, {}, false, false, {}}), "library refreshed");
    check(model.state().games[0].description == "A synthetic game.", "a library refresh keeps the store description");
}

} // namespace

const z3cloudplay::homebrew::ViewRow* saves_row(const Simulation& model, const std::string& key) {
    for (const auto& row : model.view().rows) if (row.key == key) return &row;
    return nullptr;
}

void live_saves_tab() {
    using z3cloudplay::homebrew::LiveCloudGame;
    using z3cloudplay::homebrew::LiveSavesJob;
    using z3cloudplay::homebrew::SavesJobKind;
    auto model = make_live();
    model.SetWallClock(1'800'000'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Alpha"), game(43, "Beta"), game(44, "Gamma")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateInstalls({}, {{42, 600'000'000}, {43, 100'000'000}});
    model.UpdateKeptBottles({{44, "Gamma", 1'700'000'000, 1'900'000'000}});

    // The tab sits between Downloads and Settings; opening it asks for a quick check.
    model.action("l1");
    model.action("l1");
    check(model.state().navigation.route == Route::Saves, "Saves is the tab before Settings");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::QuickCheck, "opening Saves checks what may have changed");

    LiveCloudGame first{42, CloudStatus::Conflict, "Both changed.", 2, 3, 900'000, 1'200'000, 1'799'000'000, 1'799'900'000};
    first.first_sync = true;
    LiveCloudGame synced{43, CloudStatus::Synced, "1 save uploaded", 4, 4, 312'000, 312'000};
    synced.last_sync = 1'800'000'000 - 7200;
    LiveCloudGame kept{44, CloudStatus::UpToDate, "Saves are up to date", 6, 6, 420'000, 420'000};
    kept.background = true;
    model.UpdateCloud({first, synced, kept});
    check(saves_row(model, "save-42") && saves_row(model, "save-42")->value == "Choose saves" &&
              saves_row(model, "save-42")->subtitle == "First sync · both sides already have saves",
          "a first sync with saves on both sides asks for a choice");
    check(saves_row(model, "save-43")->value == "In sync" && saves_row(model, "save-43")->badge == "Synced 2 h ago" &&
              saves_row(model, "save-43")->aside == "312 KB",
          "a synced game says when");
    check(saves_row(model, "save-44")->subtitle == "Not installed", "kept data of an uninstalled game is listed");
    check(saves_row(model, "saves-filter-1")->value == "1" && saves_row(model, "saves-filter-4")->value == "1", "filters count");

    // Chips and Sync all form one row above the list.
    check(model.action("focus:saves-filter-0"), "focus the first filter");
    model.action("down");
    check(model.state().navigation.focus_key == "save-42", "Down enters the list");
    model.action("up");
    check(model.state().navigation.focus_key == "saves-filter-0", "Up returns to the current filter");
    activate(model, "saves-filter-1");
    check(saves_row(model, "save-42") && !saves_row(model, "save-43") && !saves_row(model, "save-44"), "Needs you shows only choices");
    activate(model, "saves-filter-0");

    // A choice found again by a later check is not announced again.
    LiveCloudGame checking_again = first;
    checking_again.status = CloudStatus::Checking;
    model.UpdateCloud({checking_again});
    const auto notices_before = model.state().notices.size();
    model.UpdateCloud({first});
    check(model.state().notices.size() == notices_before, "a known choice is not announced again");

    // Sync all, and Stop while it runs; a finished Sync all says what happened.
    check(model.action("triangle"), "Triangle works on Saves");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::SyncAll, "Triangle starts Sync all");
    model.UpdateSavesJob({SavesJobKind::SyncAll, 1, 3, 43, false});
    check(saves_row(model, "saves-sync-all")->title == "Stop" && model.view().subtitle.find("Syncing 2 of 3") == 0,
          "a running Sync all shows its progress and Stop");
    activate(model, "saves-sync-all");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::StopJob, "Stop stops the job");
    model.UpdateSavesJob({});
    check(saves_row(model, "saves-sync-all")->title == "Sync all", "Sync all comes back");
    bool announced = false;
    for (const auto& notice : model.state().notices) announced |= notice.id == "saves-sync-all";
    check(announced, "a finished Sync all is announced");

    // A background failure never opens a dialog; one the player asked for does.
    LiveCloudGame failed{43, CloudStatus::Failed, "Steam is not reachable. Try again."};
    failed.background = true;
    model.UpdateCloud({failed});
    check(model.state().navigation.modals.empty() && saves_row(model, "save-43")->value == "Couldn't sync",
          "a background failure shows only in the list");

    // A game's page: the choice goes to the service; Circle returns to its row.
    activate(model, "save-42");
    check(model.state().navigation.route == Route::SaveDetails && has_row(model, "saves-use-cloud") && has_row(model, "saves-use-local"),
          "a first sync offers both sides");
    activate(model, "saves-use-cloud");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::UseCloud && requests[0].game_id == 42, "the choice is sent");
    model.action("circle");
    check(model.state().navigation.route == Route::Saves && model.state().navigation.focus_key == "save-42", "Circle returns to the row");

    // Deleting an installed game's saves asks first.
    model.UpdateCloud({synced});
    activate(model, "save-43");
    activate(model, "saves-delete");
    check(model.state().navigation.modals.back().title == "Delete saves on this console?" &&
              model.state().navigation.modals.back().body.find("Steam Cloud keeps its copy") != std::string::npos,
          "deleting saves asks and says Cloud keeps a copy");
    activate(model, "saves-delete-confirm");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::RemoveLocal && requests[0].game_id == 43, "delete is sent");
    model.action("circle");

    // An uninstalled game's leftovers: removing them frees the kept data.
    activate(model, "save-44");
    check(!has_row(model, "saves-delete") && has_row(model, "saves-leftover"), "an uninstalled game offers Remove leftover data");
    activate(model, "saves-leftover");
    check(model.state().navigation.modals.back().body.find("1.9 GB") != std::string::npos, "the size is shown first");
    activate(model, "saves-leftover-confirm");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::RemoveLeftover && requests[0].game_id == 44 &&
              model.state().navigation.route == Route::Saves,
          "removal is sent and the list comes back");

    // While a game is checked, its actions wait.
    LiveCloudGame queued = synced;
    queued.queued = true;
    model.UpdateCloud({queued});
    activate(model, "save-43");
    check(saves_row(model, "saves-sync") && !saves_row(model, "saves-sync")->enabled, "actions wait while the game is checked");

    // The game page's More menu leads to the same page.
    check(model.action("details:43"), "details open");
    activate(model, "details-more");
    activate(model, "manage-saves");
    check(model.state().navigation.route == Route::SaveDetails && model.state().navigation.save_id == 43 &&
              model.state().navigation.modals.empty(),
          "More > Manage saves opens the game's saves");
}

void live_completed_downloads_follow_the_queue() {
    using namespace z3cloudplay::homebrew;
    auto model = make_live();
    model.SetWallClock(100'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Finished"), game(43, "Downloading"), game(45, "Earlier"), game(46, "Removed")},
                               false, {}, false, false, {}}), "library loaded");
    // 46 is listed by the service but no longer installed; 43 is downloading again.
    model.UpdateInstalls({{43, JobState::Downloading, 10, 100, {}, "internal"}},
                         {{42, 100, "internal", true}, {43, 50, "internal", true}, {45, 300, "m2", true}},
                         {{42, 100, "internal", 99'880}, {45, 300, "m2", 92'800}, {43, 50, "internal", 99'000}, {46, 9, "internal", 99'990}});
    const auto& jobs = model.state().jobs;
    check(jobs.size() == 3 && jobs[0].game_id == 43 && jobs[0].state == JobState::Downloading, "the queue comes first");
    check(jobs[1].game_id == 42 && jobs[1].state == JobState::Downloaded && jobs[1].finished_at == 99'880 &&
              jobs[1].completed_bytes == 100 && jobs[1].total_bytes == 100 && jobs[2].game_id == 45 && jobs[2].volume_id == "m2",
          "finished downloads follow it, newest first, without games downloading again or uninstalled");
    check(model.state().volumes.empty() || std::all_of(model.state().volumes.begin(), model.state().volumes.end(),
                                                       [](const Volume& volume) { return volume.reserved_bytes == 0 || volume.id == "internal"; }),
          "finished downloads reserve no space");

    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open");
    check(has_row(model, "job-43") && has_row(model, "job-42") && has_row(model, "job-45"), "every entry has a row");
    activate(model, "job-42");
    check(has_row(model, "job-open-42") && has_row(model, "job-dismiss-42") && has_row(model, "job-clear-completed") &&
              !has_row(model, "job-cancel-42"), "a finished download offers Open, Dismiss and Clear completed");
    activate(model, "job-dismiss-42");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::DismissCompleted && requests[0].game_id == 42,
          "Dismiss tells the download service");
    check(!has_row(model, "job-42") && has_row(model, "job-45"), "the dismissed entry goes at once");

    activate(model, "job-45");
    check(!has_row(model, "job-clear-completed"), "one finished download needs no Clear completed");
    model.action("circle");
    model.UpdateInstalls({}, {{42, 100, "internal", true}, {43, 50, "internal", true}, {45, 300, "m2", true}},
                         {{43, 50, "internal", 99'950}, {45, 300, "m2", 92'800}});
    activate(model, "job-43");
    activate(model, "job-clear-completed");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::ClearCompleted, "Clear completed tells the service");
    check(std::none_of(model.state().jobs.begin(), model.state().jobs.end(),
                       [](const Job& job) { return job.state == JobState::Downloaded; }) && has_row(model, "downloads-empty"),
          "Clear completed empties the list");
}

void game_collections_sheet_adds_and_removes_the_game() {
    using namespace z3cloudplay::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(42, "Metro"), game(43, "Other")}, false, {}, false, false, {}}), "library loaded");
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross"), "game actions open");
    activate(model, "collection");
    check(model.state().navigation.modals.back().id == "collections" && has_row(model, "collection-new") &&
              !has_row(model, "collection-manage") && find_row(model, "collection-new")->title == "New collection",
          "with no collections the sheet offers New collection");

    activate(model, "collection-new");
    model.text("To test");
    check(model.action("done"), "the name is entered");
    const auto& collections = model.state().library.collections;
    check(collections.size() == 1 && collections[0].name == "To test" && collections[0].members.contains(42),
          "a collection made from a game's sheet holds that game");
    const auto toggle = "collection-toggle-" + collections[0].id;
    const auto* row = find_row(model, toggle);
    check(row && row->title == "To test" && row->selected && row->value == "1 game" &&
              model.state().navigation.focus_key == toggle && has_row(model, "collection-manage"),
          "the new collection shows in the open sheet, checked and focused");

    check(model.action("cross"), "the checkbox toggles");
    check(!model.state().library.collections[0].members.contains(42) && !find_row(model, toggle)->selected &&
              model.state().navigation.modals.back().id == "collections",
          "unchecking removes the game and keeps the sheet open");

    activate(model, "collection-manage");
    const auto id = model.state().library.collections[0].id;
    check(find_row(model, "collection-rename-" + id)->title == "Rename \u201cTo test\u201d" &&
              find_row(model, "collection-pin-" + id) && find_row(model, "collection-delete-" + id)->dangerous,
          "Manage collections renames, pins and deletes each collection");
    activate(model, "collection-rename-" + id);
    model.text(" later");
    check(model.action("done"), "the new name is entered");
    check(model.state().library.collections[0].name == "To test later" &&
              find_row(model, "collection-rename-" + id)->title == "Rename \u201cTo test later\u201d",
          "a rename shows in the open sheet");
}

void live_download_card_keeps_queue_order() {
    using namespace z3cloudplay::homebrew;
    auto model = make_live();
    // The download retried a moment ago waits in the queue; a failed one behind it must not take the card.
    model.UpdateInstalls({{42, JobState::Queued, 0, 100, {}}, {43, JobState::Failed, 0, 100, "Could not reach Steam."}}, {});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open");
    check(!model.view().rows.empty() && model.view().rows[0].key == "job-42" && model.view().rows[1].key == "job-43",
          "the first download in the queue keeps the top spot ahead of a failed one");
    model.UpdateInstalls({{42, JobState::Queued, 0, 100, {}}, {43, JobState::Downloading, 5, 100, {}}}, {});
    check(model.view().rows[0].key == "job-43", "a running download takes the top spot");
}

void live_download_reorder_reaches_service() {
    using namespace z3cloudplay::homebrew;
    auto model = make_live();
    model.UpdateInstalls({{43, JobState::Downloading, 10, 100, {}},
                         {44, JobState::Queued, 0, 100, {}},
                         {45, JobState::Queued, 0, 100, {}}}, {});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open for reordering");
    activate(model, "job-44");
    activate(model, "job-down-44");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].game_id == 44 && requests[0].kind == InstallRequestKind::MoveDown,
          "Move down sends the order change to the download service");
    activate(model, "job-45");
    activate(model, "job-up-45");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].game_id == 45 && requests[0].kind == InstallRequestKind::MoveUp,
          "Move up sends the order change to the download service");
}

int main() {
    live_download_reorder_reaches_service();
    live_play_does_the_next_useful_thing();
    live_store_details_fill_the_description();
    live_ready_to_play_quits_to_home();
    live_details_focus_moves_to_play_once_enabled();
    live_settings_focus_stays_near_a_removed_row();
    preparation_steps_read_as_player_words();
    live_install_requests_and_projection();
    live_install_volumes();
    live_uninstall_removes_title_and_keeps_saves();
    live_install_confirmation_checks_space_and_unknowns();
    live_install_notifications();
    live_completed_downloads_follow_the_queue();
    live_download_card_keeps_queue_order();
    game_collections_sheet_adds_and_removes_the_game();
    live_log_viewer();
    live_cloud_saves();
    live_saves_tab();
    live_filters_only_use_available_facts();
    projects_only_owned_facts_and_delivery_status();
    loading_and_failure_preserve_last_good_snapshot();
    rejects_invalid_snapshots_transactionally_and_leaves_simulation_alone();
    preserves_identity_and_scroll_then_chooses_a_stable_neighbor();
    live_details_keep_personal_edits_and_block_fixture_operations();
    refresh_actions_preserve_authentication_and_wall_clock_is_external();
    std::cout << "homebrew_library_model_test passed\n";
    return 0;
}
