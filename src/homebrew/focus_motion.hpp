// focus_motion.hpp - Bounded, renderer-neutral focus lift motion.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace z3cloudplay::homebrew {

// A per-visible-item spring for poster focus. The amount (0..1) drives the lift and the
// fade of the focus ring, glow and title overlay, so moving focus cross-fades the old and
// new cells as the macOS app's 0.18 s ease-out does. Critical damping keeps it from
// overshooting. The response/damping conversion and 1/480 s integrator match rf::Spring.
class FocusMotion {
public:
    static constexpr float focused_scale = 1.08F;
    static constexpr float response = 0.24F;
    static constexpr float damping = 1.0F;

    void begin_frame(float dt, bool reduced_motion) {
        dt_ = std::isfinite(dt) ? std::clamp(dt, 0.0F, 0.1F) : 0.0F;
        reduced_motion_ = reduced_motion;
        for (auto& [key, item] : items_) {
            (void)key;
            item.seen = false;
            item.advanced = false;
        }
    }

    // Marks a key as visible, retargets it, and returns its scale for this frame.
    float focus(std::string_view key, bool focused) {
        return 1.0F + (focused_scale - 1.0F) * amount(key, focused);
    }

    // Marks a key as visible, retargets it, and returns how focused it is (0..1).
    float amount(std::string_view key, bool focused) {
        auto [position, inserted] = items_.try_emplace(std::string(key));
        Item& item = position->second;
        if (inserted) item.spring.snap(0.0F);
        item.seen = true;
        item.spring.target = focused ? 1.0F : 0.0F;

        if (reduced_motion_) {
            item.spring.snap(item.spring.target);
            item.advanced = true;
        } else if (!item.advanced) {
            item.spring.step(dt_);
            if (item.spring.settled()) item.spring.snap(item.spring.target);
            item.advanced = true;
        }
        return std::clamp(item.spring.value, 0.0F, 1.0F);
    }

    // Drops channels for virtualized cells that were not drawn in this frame.
    void end_frame() {
        for (auto it = items_.begin(); it != items_.end();) {
            if (!it->second.seen) it = items_.erase(it);
            else ++it;
        }
    }

    std::size_t tracked() const { return items_.size(); }

private:
    struct Spring {
        float value = 0.0F;
        float velocity = 0.0F;
        float target = 0.0F;

        void snap(float next) {
            value = next;
            velocity = 0.0F;
            target = next;
        }

        void step(float dt) {
            constexpr float two_pi = 6.2831853F;
            constexpr float substep = 1.0F / 480.0F;
            constexpr float angular_frequency = two_pi / response;
            constexpr float stiffness = angular_frequency * angular_frequency;
            constexpr float friction = 2.0F * two_pi * damping / response;
            while (dt > 0.0F) {
                const float h = std::min(dt, substep);
                velocity += (-stiffness * (value - target) - friction * velocity) * h;
                value += velocity * h;
                dt -= h;
            }
        }

        bool settled() const {
            constexpr float epsilon = 1e-3F;
            return std::fabs(value - target) < epsilon && std::fabs(velocity) < epsilon;
        }
    };

    struct Item {
        Spring spring;
        bool seen = false;
        bool advanced = false;
    };

    std::unordered_map<std::string, Item> items_;
    float dt_ = 0.0F;
    bool reduced_motion_ = false;
};

} // namespace z3cloudplay::homebrew
