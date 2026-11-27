/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PREFBAR_HORIZONTAL_LAYOUT_HPP
#define PREFBAR_HORIZONTAL_LAYOUT_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace prefbar {

/*
 * Hash the row's non-numeric structure. Numeric runs are normalized so a
 * value changing from one to three digits does not create a new slot.
 */
inline std::uint32_t stable_row_signature(const char* text) noexcept {
    std::uint32_t hash = 2166136261u;
    bool in_number = false;
    if (!text)
        return hash;

    const auto mix = [&hash](std::uint8_t value) noexcept {
        hash ^= value;
        hash *= 16777619u;
    };
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        const bool numeric = (*p >= '0' && *p <= '9') || *p == '.' ||
                             *p == '-' || *p == '+';
        if (numeric) {
            if (!in_number)
                mix(static_cast<std::uint8_t>('#'));
            in_number = true;
        } else {
            mix(*p);
            in_number = false;
        }
    }
    return hash;
}

/*
 * Compact rows pack several readings into one string ("CPU: 45% | 62C |
 * 3500MHz"). A single PUI label cannot keep the later readings still when an
 * earlier number gains or loses a digit, so the renderer draws every reading in
 * its own label and gives each one its own reserved slot.
 *
 * Splitting happens at " | ". The bar stays with the reading that follows it
 * (segment text is "| 62C"), so every separator sits at the left edge of a
 * fixed slot. At most max_segments pieces are produced; the final piece
 * keeps whatever text remains. Offsets are relative to text.
 */
constexpr std::size_t kMaxRowSegments = 4;

struct RowSegments {
    std::size_t count{};
    std::size_t begin[kMaxRowSegments]{};
    std::size_t length[kMaxRowSegments]{};
};

inline RowSegments split_row_segments(
    const char* text, std::size_t max_segments = kMaxRowSegments) noexcept {

    RowSegments out;
    if (!text || !text[0])
        return out;
    if (max_segments < 1)
        max_segments = 1;
    if (max_segments > kMaxRowSegments)
        max_segments = kMaxRowSegments;

    std::size_t length = 0;
    while (text[length])
        ++length;

    std::size_t start = 0;
    for (std::size_t i = 0; i + 2 < length && out.count + 1 < max_segments; ++i) {
        if (text[i] == ' ' && text[i + 1] == '|' && text[i + 2] == ' ' && i > start) {
            out.begin[out.count] = start;
            out.length[out.count] = i - start;
            ++out.count;
            start = i + 1;   /* the '|' opens the next segment */
        }
    }
    out.begin[out.count] = start;
    out.length[out.count] = length - start;
    ++out.count;
    return out;
}

/* Gap between the readings of one row; roughly one space character. */
inline float intra_row_gap(int font_px) noexcept {
    return static_cast<float>(font_px) * 0.30f;
}

/* Resolve a right-anchored row from its reserved slot width. Invalid geometry
 * falls back to the left margin rather than propagating NaN into a PUI Widget.
 */
inline float right_aligned_slot_x(
    float screen_width, float margin_x, float slot_width) noexcept {
    if (!std::isfinite(margin_x) || margin_x < 0.0f)
        return 0.0f;
    if (!std::isfinite(screen_width) || !std::isfinite(slot_width) ||
        slot_width < 0.0f)
        return margin_x;
    const float x = screen_width - margin_x - slot_width;
    return std::isfinite(x) && x > margin_x ? x : margin_x;
}

/*
 * Retain the widest measured text for each layout row. PUI labels use
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
        signatures_.fill(0u);
        initialized_.fill(false);
    }

    float reserve(std::size_t index, std::uint32_t signature, float measured) noexcept {
        if (index >= Count)
            return 0.0f;
        if (!initialized_[index] || signatures_[index] != signature) {
            initialized_[index] = true;
            signatures_[index] = signature;
            widths_[index] = std::isfinite(measured) && measured > 0.0f
                ? measured : 0.0f;
        } else if (std::isfinite(measured) && measured > widths_[index]) {
            widths_[index] = measured;
        }
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
    std::array<std::uint32_t, Count> signatures_{};
    std::array<bool, Count> initialized_{};
};

} // namespace prefbar

#endif // PREFBAR_HORIZONTAL_LAYOUT_HPP
