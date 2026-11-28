/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Host regression tests for stable telemetry row widths.
 */
#include "prefbar/horizontal_layout.hpp"

#include <cstdio>
#include <cstring>
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
    const auto first = prefbar::stable_row_signature("FPS 59.8 | FT 9.9ms");
    const auto wider = prefbar::stable_row_signature("FPS 100.0 | FT 10.0ms");
    const auto different = prefbar::stable_row_signature("CPU: 72% | 70C");
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

void test_vertical_right_anchor_uses_stable_slot() {
    prefbar::StableWidthCache<1> cache;
    const auto signature = prefbar::stable_row_signature("CPU: 72% | 70C | 2400MHz");

    const float initial_width = cache.reserve(0, signature, 132.0f);
    const float initial_x = prefbar::right_aligned_slot_x(1920.0f, 24.0f, initial_width);

    const float expanded_width = cache.reserve(0, signature, 148.0f);
    const float expanded_x = prefbar::right_aligned_slot_x(1920.0f, 24.0f, expanded_width);
    CHECK(expanded_width == 148.0f && expanded_x < initial_x,
          "a newly observed widest value may expand the reserved vertical slot");

    const float shorter_width = cache.reserve(0, signature, 118.0f);
    const float shorter_x = prefbar::right_aligned_slot_x(1920.0f, 24.0f, shorter_width);
    CHECK(shorter_width == expanded_width,
          "shorter compact vertical values must not shrink their slot");
    CHECK(shorter_x == expanded_x,
          "right-anchored vertical row position must not jitter after shrinking values");
}

void test_right_anchor_geometry_fails_safe() {
    CHECK(prefbar::right_aligned_slot_x(1920.0f, 24.0f, 1900.0f) == 24.0f,
          "oversized rows should clamp to the configured margin");
    CHECK(prefbar::right_aligned_slot_x(
              std::numeric_limits<float>::quiet_NaN(), 24.0f, 100.0f) == 24.0f,
          "invalid screen width should fall back to the left margin");
    CHECK(prefbar::right_aligned_slot_x(
              1920.0f, 24.0f, std::numeric_limits<float>::infinity()) == 24.0f,
          "invalid slot width should fall back to the left margin");
    CHECK(prefbar::right_aligned_slot_x(1920.0f, -1.0f, 100.0f) == 0.0f,
          "invalid negative margin should fail safely");
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

bool segment_is(const char* text, const prefbar::RowSegments& s,
                std::size_t index, const char* expected) {
    return index < s.count &&
           s.length[index] == std::strlen(expected) &&
           std::strncmp(text + s.begin[index], expected, s.length[index]) == 0;
}

void test_compact_row_splits_into_independent_readings() {
    const char* row = "CPU: 45% | 62C | 3500MHz";
    const auto s = prefbar::split_row_segments(row);
    CHECK(s.count == 3, "three readings expected, got %zu", s.count);
    CHECK(segment_is(row, s, 0, "CPU: 45%"), "first reading keeps its label");
    CHECK(segment_is(row, s, 1, "| 62C"), "separator stays with the following reading");
    CHECK(segment_is(row, s, 2, "| 3500MHz"), "last reading keeps its separator");
}

void test_segment_signatures_ignore_digit_count() {
    const char* narrow = "CPU: 9% | 62C | 800MHz";
    const char* wide = "CPU: 100% | 100C | 3500MHz";
    const auto a = prefbar::split_row_segments(narrow);
    const auto b = prefbar::split_row_segments(wide);
    CHECK(a.count == b.count && a.count == 3, "segment count must not depend on digits");
    for (std::size_t i = 0; i < a.count; ++i) {
        char left[96]{};
        char right[96]{};
        std::memcpy(left, narrow + a.begin[i], a.length[i]);
        std::memcpy(right, wide + b.begin[i], b.length[i]);
        CHECK(prefbar::stable_row_signature(left) == prefbar::stable_row_signature(right),
              "segment %zu must keep one slot identity across digit changes", i);
    }
}

void test_segment_slots_keep_following_readings_still() {
    /* The 62C reading must stay put while the CPU percentage grows and shrinks. */
    prefbar::StableWidthCache<3> cache;
    const char* rows[] = {"CPU: 9% | 62C", "CPU: 45% | 62C", "CPU: 100% | 62C", "CPU: 7% | 62C"};
    const float widths[][2] = {{70.0f, 48.0f}, {82.0f, 48.0f}, {96.0f, 48.0f}, {64.0f, 48.0f}};
    float first_x = -1.0f;
    for (int n = 0; n < 4; ++n) {
        const auto s = prefbar::split_row_segments(rows[n]);
        float x = 0.0f;
        float second_x = 0.0f;
        for (std::size_t i = 0; i < s.count; ++i) {
            char text[96]{};
            std::memcpy(text, rows[n] + s.begin[i], s.length[i]);
            const float slot = cache.reserve(
                i, prefbar::stable_row_signature(text), widths[n][i]);
            if (i == 1)
                second_x = x;
            x += slot + prefbar::intra_row_gap(26);
        }
        if (n == 0) {
            first_x = second_x;
        } else if (n >= 2) {
            CHECK(second_x >= first_x, "slots may only grow");
        }
        if (n == 3) {
            CHECK(second_x == cache.get(0) + prefbar::intra_row_gap(26),
                  "62C must not move when the CPU value shrinks again");
        }
    }
}

void test_segment_splitting_edge_cases() {
    CHECK(prefbar::split_row_segments(nullptr).count == 0, "null text has no segments");
    CHECK(prefbar::split_row_segments("").count == 0, "empty text has no segments");

    const char* plain = "FPS 60.0";
    const auto one = prefbar::split_row_segments(plain);
    CHECK(one.count == 1 && segment_is(plain, one, 0, "FPS 60.0"),
          "a row without separators stays one segment");

    const char* lead = " | 62C";
    const auto leading = prefbar::split_row_segments(lead);
    CHECK(leading.count == 1 && segment_is(lead, leading, 0, " | 62C"),
          "a leading separator must not create an empty segment");

    const char* four = "A | B | C | D | E";
    const auto capped = prefbar::split_row_segments(four, 2);
    CHECK(capped.count == 2 && segment_is(four, capped, 0, "A") &&
              segment_is(four, capped, 1, "| B | C | D | E"),
          "the last permitted segment keeps the remaining text");
    const auto limited = prefbar::split_row_segments(four);
    CHECK(limited.count == prefbar::kMaxRowSegments,
          "segment count must never exceed the compile-time limit");

    const char* bars = "cores: 10 20 30 | |";
    const auto odd = prefbar::split_row_segments(bars);
    CHECK(odd.count > 0 && odd.count <= prefbar::kMaxRowSegments,
          "repeated separators must remain within the segment limit");
}


void test_estimates_never_become_the_permanent_slot() {
    prefbar::StableWidthCache<2> cache;
    /* Width unreadable on the first frame: a generous estimate stands in. */
    CHECK(cache.reserve_estimate(0, 0x11u, 240.0f) == 240.0f,
          "an estimate may be used until a real width exists");
    CHECK(cache.reserve_estimate(0, 0x11u, 250.0f) == 250.0f,
          "estimates may still grow while nothing was measured");
    /* The first real measurement replaces it, even though it is smaller. */
    CHECK(cache.reserve(0, 0x11u, 190.0f) == 190.0f,
          "a real width must replace the estimate instead of being capped by it");
    CHECK(cache.reserve_estimate(0, 0x11u, 300.0f) == 190.0f,
          "once measured, estimates are ignored");
    CHECK(cache.reserve(0, 0x11u, 175.0f) == 190.0f,
          "measured widths keep their high-water behaviour");
    CHECK(cache.reserve(0, 0x11u, 205.0f) == 205.0f,
          "measured widths still grow");
    CHECK(cache.reserve_estimate(1, 0x22u, std::numeric_limits<float>::quiet_NaN()) == 0.0f,
          "invalid estimates reserve nothing");
}
} // namespace

int main() {
    test_widths_only_grow_during_updates();
    test_rows_have_independent_slots();
    test_font_scaling_preserves_reserved_geometry();
    test_row_signature_tracks_structure_not_values();
    test_vertical_right_anchor_uses_stable_slot();
    test_right_anchor_geometry_fails_safe();
    test_reset_and_invalid_measurements();
    test_compact_row_splits_into_independent_readings();
    test_segment_signatures_ignore_digit_count();
    test_segment_slots_keep_following_readings_still();
    test_segment_splitting_edge_cases();
    test_estimates_never_become_the_permanent_slot();
    if (failures != 0) {
        std::printf("%d layout test(s) failed\n", failures);
        return 1;
    }
    std::puts("All telemetry layout tests passed");
    return 0;
}
