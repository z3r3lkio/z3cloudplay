// homebrew_focus_motion_test.cpp - Renderer-neutral focus lift motion tests.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homebrew/focus_motion.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

float frame(z3cloudplay::homebrew::FocusMotion& motion, float dt, bool focused,
            bool reduced_motion = false) {
    motion.begin_frame(dt, reduced_motion);
    const float scale = motion.focus("poster", focused);
    motion.end_frame();
    return scale;
}

void focus_grows_from_rest() {
    z3cloudplay::homebrew::FocusMotion motion;
    const float scale = frame(motion, 1.0F / 60.0F, true);
    check(scale > 1.0F && scale < 1.08F,
          "focus starts the Ember poster lift without jumping to its target");
}

void retarget_keeps_the_current_value() {
    z3cloudplay::homebrew::FocusMotion motion;
    float scale = 1.0F;
    for (int i = 0; i < 4; ++i) scale = frame(motion, 1.0F / 60.0F, true);

    motion.begin_frame(0.0F, false);
    const float retargeted = motion.focus("poster", false);
    motion.end_frame();
    check(std::fabs(retargeted - scale) < 1e-6F,
          "defocus retarget preserves the in-flight scale");

    const float next = frame(motion, 1.0F / 60.0F, false);
    check(next > 1.0F, "defocus continues smoothly instead of resetting to rest");
}

void spring_settles_exactly_at_each_target() {
    z3cloudplay::homebrew::FocusMotion motion;
    float scale = 1.0F;
    for (int i = 0; i < 240; ++i) scale = frame(motion, 1.0F / 60.0F, true);
    check(std::fabs(scale - 1.08F) < 1e-6F,
          "focused Ember poster settles exactly at 1.08 scale");

    for (int i = 0; i < 240; ++i) scale = frame(motion, 1.0F / 60.0F, false);
    check(std::fabs(scale - 1.0F) < 1e-6F,
          "defocused poster settles exactly at rest scale");
}

void fixed_substeps_are_stable_across_frame_rates() {
    z3cloudplay::homebrew::FocusMotion sixty_hz;
    z3cloudplay::homebrew::FocusMotion thirty_hz;
    float sixty_scale = 1.0F;
    float thirty_scale = 1.0F;
    for (int i = 0; i < 60; ++i) sixty_scale = frame(sixty_hz, 1.0F / 60.0F, true);
    for (int i = 0; i < 30; ++i) thirty_scale = frame(thirty_hz, 1.0F / 30.0F, true);
    check(std::fabs(sixty_scale - thirty_scale) < 2e-4F,
          "equal elapsed time gives the same lift at 30 and 60 Hz");
}

void reduced_motion_snaps_and_offscreen_entries_are_pruned() {
    z3cloudplay::homebrew::FocusMotion motion;
    check(std::fabs(frame(motion, 1.0F / 60.0F, true, true) - 1.08F) < 1e-6F,
          "reduced motion snaps directly to focused scale");
    check(std::fabs(frame(motion, 1.0F / 60.0F, false, true) - 1.0F) < 1e-6F,
          "reduced motion snaps directly to rest scale");

    motion.begin_frame(1.0F / 60.0F, false);
    motion.focus("visible-a", false);
    motion.focus("visible-b", true);
    motion.end_frame();
    check(motion.tracked() == 2, "only keys seen in the current frame remain tracked");

    motion.begin_frame(1.0F / 60.0F, false);
    motion.focus("visible-b", true);
    motion.end_frame();
    check(motion.tracked() == 1, "an offscreen key is pruned at frame end");
}

} // namespace

int main() {
    focus_grows_from_rest();
    retarget_keeps_the_current_value();
    spring_settles_exactly_at_each_target();
    fixed_substeps_are_stable_across_frame_rates();
    reduced_motion_snaps_and_offscreen_entries_are_pruned();
    std::cout << "homebrew_focus_motion_test passed\n";
}
