// input_adapter.hpp - Map kit pad events to renderer-free diagnostics.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "homebrew/simulation.hpp"
#include "core/input.hpp"

namespace z3cloudplay::homebrew {
inline ControllerState diagnostic_sample(const hui::InputFrame& frame) {
    ControllerState sample;
    sample.left_x = frame.stick_x; sample.left_y = frame.stick_y;
    sample.right_x = frame.stick2_x; sample.right_y = frame.stick2_y;
    sample.l2 = frame.trigger_l; sample.r2 = frame.trigger_r;
    sample.cross = frame.is_held(hui::Action::confirm);
    sample.circle = frame.is_held(hui::Action::back);
    sample.square = frame.is_held(hui::Action::west);
    sample.triangle = frame.is_held(hui::Action::north);
    sample.l1 = frame.is_held(hui::Action::page_prev);
    sample.r1 = frame.is_held(hui::Action::page_next);
    sample.options = frame.is_held(hui::Action::menu);
    return sample;
}
}
