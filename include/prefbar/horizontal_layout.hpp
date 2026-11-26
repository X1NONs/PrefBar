/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PREFBAR_HORIZONTAL_LAYOUT_HPP
#define PREFBAR_HORIZONTAL_LAYOUT_HPP

#include <array>
#include <cmath>
#include <cstddef>

namespace prefbar {

/*
 * Retain the widest measured text for each horizontal row. PUI labels use
 * FitWidthToText, so their measured widths can fluctuate as telemetry changes.
 * Keeping each slot at its high-water mark prevents repeated back-and-forth
 * movement. A slot can expand when a genuinely wider value appears, but never
 * shrinks until the layout is rebuilt.
 */
template <std::size_t Count>
class StableWidthCache {
public:
    void reset() noexcept {
        widths_.fill(0.0f);
    }

    float reserve(std::size_t index, float measured) noexcept {
        if (index >= Count)
            return 0.0f;
        if (std::isfinite(measured) && measured > widths_[index])
            widths_[index] = measured;
        return widths_[index];
    }

    void scale(float factor) noexcept {
        if (!std::isfinite(factor) || factor <= 0.0f)
            return;
        for (float& width : widths_)
            width *= factor;
    }

    float get(std::size_t index) const noexcept {
        return index < Count ? widths_[index] : 0.0f;
    }

private:
    std::array<float, Count> widths_{};
};

} // namespace prefbar

#endif // PREFBAR_HORIZONTAL_LAYOUT_HPP
