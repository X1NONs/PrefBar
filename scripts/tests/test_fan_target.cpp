/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Host tests for the optional PS5 automatic fan target setting.
 */
#include "prefbar/fan_target.h"

#include <cstdio>

namespace {
int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL line %d: %s\n", __LINE__, message); \
    } \
} while (0)

void test_system_mode() {
    int value = -1;
    CHECK(prefbar_fan_target_parse("system", &value) &&
              value == PREFBAR_FAN_TARGET_SYSTEM,
          "system must disable PrefBar fan writes");
    CHECK(prefbar_fan_target_parse("auto", &value) &&
              value == PREFBAR_FAN_TARGET_SYSTEM,
          "auto must be accepted as a system-mode alias");
    CHECK(prefbar_fan_target_parse("SYSTEM", &value) &&
              value == PREFBAR_FAN_TARGET_SYSTEM,
          "system parsing should be case-insensitive");
}

void test_supported_numeric_range() {
    int value = -1;
    CHECK(prefbar_fan_target_parse("50", &value) && value == 50,
          "lower supported target must parse");
    CHECK(prefbar_fan_target_parse("75", &value) && value == 75,
          "middle supported target must parse");
    CHECK(prefbar_fan_target_parse("91", &value) && value == 91,
          "upper supported target must parse");
    CHECK(prefbar_fan_target_valid(PREFBAR_FAN_TARGET_SYSTEM),
          "system sentinel must be valid");
}

void test_invalid_values_are_rejected() {
    int value = 123;
    CHECK(!prefbar_fan_target_parse(nullptr, &value),
          "null input must be rejected");
    CHECK(!prefbar_fan_target_parse("", &value),
          "empty input must be rejected");
    CHECK(!prefbar_fan_target_parse("49", &value),
          "values below the supported range must be rejected");
    CHECK(!prefbar_fan_target_parse("92", &value),
          "values above the supported range must be rejected");
    CHECK(!prefbar_fan_target_parse("-1", &value),
          "signed values must be rejected");
    CHECK(!prefbar_fan_target_parse("75C", &value),
          "suffixes must be rejected");
    CHECK(!prefbar_fan_target_parse(" 75", &value),
          "untrimmed whitespace must be rejected");
    CHECK(!prefbar_fan_target_parse("75.5", &value),
          "fractional values must be rejected");
    CHECK(!prefbar_fan_target_parse("999999999999999999999999", &value),
          "oversized numeric strings must be rejected");
    CHECK(!prefbar_fan_target_valid(49) &&
              !prefbar_fan_target_valid(92),
          "out-of-range values must not pass validation");
}

void test_output_is_unchanged_on_failure() {
    int value = 77;
    CHECK(!prefbar_fan_target_parse("92", &value) && value == 77,
          "failed parsing must not mutate the caller's value");
}
} // namespace

int main() {
    test_system_mode();
    test_supported_numeric_range();
    test_invalid_values_are_rejected();
    test_output_is_unchanged_on_failure();
    if (failures != 0) {
        std::printf("%d fan target test(s) failed\n", failures);
        return 1;
    }
    std::puts("All fan target tests passed");
    return 0;
}
