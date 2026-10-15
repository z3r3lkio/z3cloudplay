// homebrew_storage_test.cpp - Storage simulation accounting tests.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#include "homebrew/storage_sim.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace z3cloudplay::homebrew;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Game game(GameId id, std::optional<std::uint64_t> download,
          std::optional<std::uint64_t> installed) {
    Game result;
    result.id = id;
    result.download_bytes = download;
    result.install_bytes = installed;
    return result;
}

Volume volume(std::string id, std::uint64_t capacity, std::uint64_t used,
              std::uint64_t reserved) {
    Volume result;
    result.id = std::move(id);
    result.capacity_bytes = capacity;
    result.used_bytes = used;
    result.reserved_bytes = reserved;
    result.base_used_bytes = used;
    result.base_reserved_bytes = reserved;
    return result;
}

Job install_job(std::string id, GameId game_id, std::string volume_id,
                std::uint64_t reservation, JobState state = JobState::Queued) {
    Job result;
    result.id = std::move(id);
    result.game_id = game_id;
    result.kind = "install";
    result.volume_id = std::move(volume_id);
    result.state = state;
    result.reserved_bytes = reservation;
    result.reservation_held = true;
    return result;
}

void assessment_uses_installed_footprint_and_preserves_unknowns() {
    State state;
    state.games.push_back(game(1, 100, 150));
    state.games.push_back(game(2, std::nullopt, std::nullopt));
    state.volumes.push_back(volume("external", 1000, 800, 50));

    auto assessment = assess_install(state, 1, "external");
    check(assessment.readiness == InstallReadiness::Ready,
          "installed footprint equal to free bytes is ready");
    check(assessment.download_bytes == 100 && assessment.installed_bytes == 150 &&
              assessment.required_bytes == 150,
          "required storage uses installed bytes rather than download bytes");
    check(assessment.free_bytes == 150 && assessment.reserved_bytes == 50,
          "assessment reports saturated free and reserved bytes");

    state.volumes[0].used_bytes = 801;
    assessment = assess_install(state, 1, "external");
    check(assessment.readiness == InstallReadiness::Insufficient,
          "installed footprint larger than free bytes is insufficient");

    assessment = assess_install(state, 2, "external");
    check(assessment.readiness == InstallReadiness::UnknownSize,
          "unknown installed footprint blocks submission");
    check(!assessment.download_bytes && !assessment.installed_bytes &&
              !assessment.required_bytes,
          "unknown sizes remain null rather than becoming zero");
}

void assessment_preserves_unknown_capacity() {
    State state;
    state.games.push_back(game(1, 100, 150));
    state.volumes.push_back(volume("external", 0, 0, 0));
    state.volumes[0].capacity_known = false;
    const auto assessment = assess_install(state, 1, "external");
    check(!assessment.free_bytes, "unknown capacity never becomes zero free bytes");
    check(assessment.readiness == InstallReadiness::UnknownCapacity,
          "unknown capacity cannot approve an install");
}

void assessment_checks_margin_even_without_size() {
    State state;
    state.games.push_back(game(1, std::nullopt, std::nullopt));
    state.volumes.push_back(volume("external", 100, 100, 0));
    check(assess_install(state, 1, "external", 10).readiness == InstallReadiness::Insufficient,
          "unknown size cannot bypass a known missing safety margin");
}

void assessment_rechecks_availability_and_writability() {
    State state;
    state.games.push_back(game(1, 100, 150));
    state.volumes.push_back(volume("external", 1000, 100, 20));

    state.volumes[0].available = false;
    check(assess_install(state, 1, "external").readiness == InstallReadiness::Unavailable,
          "disconnected destination is unavailable");
    state.volumes[0].available = true;
    state.volumes[0].writable = false;
    check(assess_install(state, 1, "external").readiness == InstallReadiness::ReadOnly,
          "read-only destination blocks submission");
    check(assess_install(state, 1, "missing").readiness == InstallReadiness::Unavailable,
          "unknown destination never invents a writable path");
}

void reconciliation_is_baseline_preserving_and_idempotent() {
    State state;
    state.volumes.push_back(volume("external", 2000, 100, 20));
    state.installations.push_back({1, "external", 200, true, true, true});
    state.jobs.push_back(install_job("job-2", 2, "external", 300));
    state.jobs.push_back(install_job("duplicate-2", 2, "external", 300, JobState::Paused));
    state.jobs.push_back(install_job("job-3", 3, "external", 100, JobState::Interrupted));
    state.jobs.push_back(install_job("job-4", 4, "external", 50, JobState::Failed));
    Job cancelled = install_job("job-5", 5, "external", 70, JobState::Cancelled);
    cancelled.reservation_held = false;
    state.jobs.push_back(cancelled);

    reconcile_storage(state);
    check(state.volumes[0].used_bytes == 300,
          "used bytes equal unrelated baseline plus exact installations");
    check(state.volumes[0].reserved_bytes == 470,
          "held reservations include one duplicate, interruption, and retryable failure");
    reconcile_storage(state);
    check(state.volumes[0].used_bytes == 300 && state.volumes[0].reserved_bytes == 470,
          "reconciliation is deterministic and idempotent");

    state.jobs[3].reservation_held = false;
    reconcile_storage(state);
    check(state.volumes[0].reserved_bytes == 420,
          "terminal failure releases only after explicit policy clears its reservation");
    state.jobs[3].reservation_held = true;
    reconcile_storage(state);
    check(state.volumes[0].reserved_bytes == 470,
          "retry reacquires the same reservation without altering baseline");
}

void completion_moves_reservation_to_used_and_uninstall_preserves_saves() {
    State state;
    state.volumes.push_back(volume("external", 2000, 100, 20));
    state.jobs.push_back(install_job("job-2", 2, "external", 300, JobState::Downloaded));
    state.jobs.push_back(install_job("duplicate-2", 2, "external", 300, JobState::Paused));

    reconcile_storage(state);
    check(state.installations.size() == 1 && state.installations[0].installed_bytes == 300,
          "completed install materializes its exact reserved footprint");
    check(!state.installations[0].runtime_ready,
          "download completion never implies runtime readiness");
    check(!state.jobs[0].reservation_held && !state.jobs[1].reservation_held,
          "completion clears all duplicate reservations for the installed game");
    check(state.volumes[0].used_bytes == 400 && state.volumes[0].reserved_bytes == 20,
          "completion moves reservation to used while preserving unrelated baseline");

    Job uninstall;
    uninstall.id = "uninstall-2";
    uninstall.game_id = 2;
    uninstall.kind = "uninstall";
    uninstall.volume_id = "external";
    uninstall.state = JobState::Downloaded;
    state.jobs.push_back(uninstall);
    reconcile_storage(state);
    check(state.installations[0].installed_bytes == 0 && state.installations[0].saves_present,
          "uninstall removes exact installed footprint while preserving saves");
    check(state.volumes[0].used_bytes == 100 && state.volumes[0].reserved_bytes == 20,
          "uninstall leaves unrelated used and reserved baselines unchanged");
    state.jobs.push_back(install_job("reinstall-2", 2, "external", 300, JobState::Downloaded));
    reconcile_storage(state);
    reconcile_storage(state);
    check(state.installations[0].installed_bytes == 300 && state.installations[0].saves_present &&
              !state.installations[0].runtime_ready && state.volumes[0].used_bytes == 400,
          "old completion records cannot repeat mutations or erase a reinstallation");
}

} // namespace

int main() {
    assessment_uses_installed_footprint_and_preserves_unknowns();
    assessment_preserves_unknown_capacity();
    assessment_checks_margin_even_without_size();
    assessment_rechecks_availability_and_writability();
    reconciliation_is_baseline_preserving_and_idempotent();
    completion_moves_reservation_to_used_and_uninstall_preserves_saves();
    std::cout << "homebrew_storage_test passed\n";
}
