// storage_sim.hpp - Deterministic storage accounting for the UI simulation.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "homebrew/simulation.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace z3cloudplay::homebrew {

enum class InstallReadiness {
    Ready,
    UnknownSize,
    UnknownCapacity,
    Insufficient,
    Unavailable,
    ReadOnly,
};

struct InstallAssessment {
    InstallReadiness readiness = InstallReadiness::Unavailable;
    std::optional<std::uint64_t> download_bytes;
    std::optional<std::uint64_t> installed_bytes;
    std::optional<std::uint64_t> required_bytes;
    std::optional<std::uint64_t> free_bytes;
    std::optional<std::uint64_t> reserved_bytes;
};

[[nodiscard]] InstallAssessment assess_install(const State& state, GameId game_id,
                                               const std::string& volume_id,
                                               std::uint64_t safety_margin_bytes = 0) noexcept;

void reconcile_storage(State& state) noexcept;

} // namespace z3cloudplay::homebrew
