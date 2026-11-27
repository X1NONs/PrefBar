/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PREFBAR_FAN_TARGET_H
#define PREFBAR_FAN_TARGET_H

#include <strings.h>

#define PREFBAR_FAN_TARGET_SYSTEM 0
#define PREFBAR_FAN_TARGET_MIN_C 50
#define PREFBAR_FAN_TARGET_MAX_C 91

static inline int prefbar_fan_target_valid(int target_c) {
    return target_c == PREFBAR_FAN_TARGET_SYSTEM ||
        (target_c >= PREFBAR_FAN_TARGET_MIN_C &&
         target_c <= PREFBAR_FAN_TARGET_MAX_C);
}

/* "system" and "auto" both mean: PrefBar does not write an override. */
static inline int prefbar_fan_target_parse(
    const char* text, int* target_c_out) {

    if (!text || !target_c_out)
        return 0;
    if (strcasecmp(text, "system") == 0 || strcasecmp(text, "auto") == 0) {
        *target_c_out = PREFBAR_FAN_TARGET_SYSTEM;
        return 1;
    }
    if (!text[0])
        return 0;

    unsigned int value = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return 0;
        const unsigned int digit = (unsigned int)(*p - '0');
        if (digit > PREFBAR_FAN_TARGET_MAX_C ||
            value > (PREFBAR_FAN_TARGET_MAX_C - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }

    if (value < PREFBAR_FAN_TARGET_MIN_C ||
        value > PREFBAR_FAN_TARGET_MAX_C)
        return 0;

    *target_c_out = (int)value;
    return 1;
}

#endif /* PREFBAR_FAN_TARGET_H */
