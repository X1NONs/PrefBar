/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Host regression tests for stable horizontal telemetry row widths.
 */
#include "prefbar/horizontal_layout.hpp"

#include <cstdio>
#include <limits>

namespace {
int failures = 0;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL line %d: ", __LINE__); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
    } \
} while (0)

void test_widths_only_grow_during_updates() {
    prefbar::StableWidthCache<3> cache;
    CHECK(cache.reserve(0, 0xA1u, 120.0f) == 120.0f,
          "first measurement should establish the slot width");
    CHECK(cache.reserve(0, 0xA1u, 112.0f) == 120.0f,
          "a shorter value must not shrink the slot");
    CHECK(cache.reserve(0, 0xA1u, 126.0f) == 126.0f,
          "a wider value must expand the slot");
    CHECK(cache.reserve(0, 0xA1u, 118.0f) == 126.0f,
          "the slot must remain stable after a wider value was observed");
}

void test_rows_have_independent_slots() {
    prefbar::StableWidthCache<3> cache;
    (void)cache.reserve(0, 0x10u, 100.0f);
    (void)cache.reserve(1, 0x20u, 220.0f);
    (void)cache.reserve(2, 0x30u, 80.0f);
    CHECK(cache.get(0) == 100.0f, "row zero width should be independent");
    CHECK(cache.get(1) == 220.0f, "row one width should be independent");
    CHECK(cache.get(2) == 80.0f, "row two width should be independent");
}

void test_font_scaling_preserves_reserved_geometry() {
    prefbar::StableWidthCache<2> cache;
    (void)cache.reserve(0, 0x10u, 200.0f);
    (void)cache.reserve(1, 0x20u, 100.0f);
    cache.scale(0.5f);
    CHECK(cache.get(0) == 100.0f && cache.get(1) == 50.0f,
          "font fitting should scale cached widths proportionally");
    CHECK(cache.reserve(0, 0x10u, 90.0f) == 100.0f,
          "scaled cache should still retain the high-water width");
    CHECK(cache.reserve(0, 0x10u, 105.0f) == 105.0f,
          "new measurements should expand the scaled slot when necessary");
}

void test_row_signature_tracks_structure_not_values() {
    const auto first = prefbar::horizontal_row_signature("FPS 59.8 | FT 9.9ms");
    const auto wider = prefbar::horizontal_row_signature("FPS 100.0 | FT 10.0ms");
    const auto different = prefbar::horizontal_row_signature("CPU: 72% | 70C");
    CHECK(first == wider,
          "numeric value changes should preserve the same row signature");
    CHECK(first != different,
          "a different metric row should receive a different signature");

    prefbar::StableWidthCache<2> cache;
    CHECK(cache.reserve(0, first, 180.0f) == 180.0f,
          "first row identity should establish its width");
    CHECK(cache.reserve(0, wider, 150.0f) == 180.0f,
          "same row with changed values should keep its width");
    CHECK(cache.reserve(0, different, 90.0f) == 90.0f,
          "new row identity should discard an unrelated old slot width");
}

void test_reset_and_invalid_measurements() {
    prefbar::StableWidthCache<1> cache;
    (void)cache.reserve(0, 0x01u, 90.0f);
    CHECK(cache.reserve(0, 0x01u, std::numeric_limits<float>::quiet_NaN()) == 90.0f,
          "NaN measurements must not poison an existing slot");
    CHECK(cache.reserve(0, 0x01u, std::numeric_limits<float>::infinity()) == 90.0f,
          "infinite measurements must not poison an existing slot");
    CHECK(cache.reserve(0, 0x01u, -1.0f) == 90.0f,
          "negative measurements must not shrink an existing slot");
    CHECK(cache.reserve(1, 0x02u, 50.0f) == 0.0f,
          "out-of-range row indices must be rejected");
    cache.scale(std::numeric_limits<float>::quiet_NaN());
    CHECK(cache.get(0) == 90.0f,
          "invalid scale factors must leave reserved widths unchanged");
    cache.reset();
    CHECK(cache.get(0) == 0.0f,
          "reset should clear widths when the layout is rebuilt");
}
} // namespace

int main() {
    test_widths_only_grow_during_updates();
    test_rows_have_independent_slots();
    test_font_scaling_preserves_reserved_geometry();
    test_row_signature_tracks_structure_not_values();
    test_reset_and_invalid_measurements();
    if (failures != 0) {
        std::printf("%d horizontal layout test(s) failed\n", failures);
        return 1;
    }
    std::puts("All horizontal layout tests passed");
    return 0;
}
