/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>

namespace prefbar {

/* Detect three distinct button-press edges inside one bounded time window.
 * A held button never counts twice; a late click starts a new sequence. */
class TouchpadTripleClick {
public:
    explicit constexpr TouchpadTripleClick(std::uint64_t window_ms = 1500) noexcept
        : window_ms_(window_ms) {}

    bool update(bool pressed, std::uint64_t now_ms) noexcept {
        bool toggled = false;
        if (pressed && !was_pressed_) {
            if (click_count_ == 0 || now_ms < first_click_ms_ ||
                now_ms - first_click_ms_ > window_ms_) {
                click_count_ = 1;
                first_click_ms_ = now_ms;
            } else {
                ++click_count_;
            }
            if (click_count_ >= 3) {
                click_count_ = 0;
                first_click_ms_ = 0;
                toggled = true;
            }
        }
        was_pressed_ = pressed;
        return toggled;
    }

    void reset() noexcept {
        was_pressed_ = false;
        click_count_ = 0;
        first_click_ms_ = 0;
    }

private:
    std::uint64_t window_ms_;
    std::uint64_t first_click_ms_ = 0;
    unsigned click_count_ = 0;
    bool was_pressed_ = false;
};

} // namespace prefbar
