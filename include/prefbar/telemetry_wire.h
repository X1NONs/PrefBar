/*
 * PrefBar - text-row packet (PrefBar telemetry -> ShellUI renderer)
 *
 * Derived from Common FPS for PS5 / ps5-dualsense-overlay (GPL-3.0-or-later):
 *   Copyright (C) 2026 porhe911, khalifa007 (SimpleFPS), erickdavestech
 * Additions Copyright (C) 2026 the prefbar-overlay authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One UDP datagram to 127.0.0.1:39028 (the port the ShellUI renderer already
 * binds). Plain C so PrefBar telemetry and the C++ renderer share it.
 */
#ifndef PREFBAR_WIRE_H
#define PREFBAR_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define PREFBAR_WIRE_MAGIC   0x52414250u   /* bytes 'P','B','A','R' (little endian) */
#define PREFBAR_WIRE_VERSION 2u
#define PREFBAR_WIRE_PORT    39028u
#define PREFBAR_CONTROL_PORT     39029u   /* ShellUI -> controller debug/control events */
#define PREFBAR_ROWS         6
#define PREFBAR_ROW_LEN      96            /* incl. NUL */
#define PREFBAR_CORNER_TL    0             /* only top-left and bottom-left are  */
#define PREFBAR_CORNER_BL    2             /* implemented; rows are left aligned */
#define PREFBAR_WIRE_BYTES   621u
#define PREFBAR_WIRE_FLAG_TEXT_SHADOW       0x01u
#define PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE   0x02u
#define PREFBAR_WIRE_FLAG_OVERLAY_ENABLED   0x04u
#define PREFBAR_WIRE_KNOWN_FLAGS (PREFBAR_WIRE_FLAG_TEXT_SHADOW | PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE | PREFBAR_WIRE_FLAG_OVERLAY_ENABLED)

#pragma pack(push, 1)
typedef struct prefbar_wire {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                 /* == sizeof(prefbar_wire_t) */
    uint64_t sequence;             /* non-zero */
    uint8_t  corner;               /* PREFBAR_CORNER_TL / PREFBAR_CORNER_BL */
    uint8_t  rows;                 /* 1..PREFBAR_ROWS used rows */
    uint8_t  flags;                /* PREFBAR_WIRE_FLAG_* */
    uint16_t font_size;            /* px, 10..96 */
    float    margin_x, margin_y;   /* px from the chosen corner */
    float    r, g, b, a;           /* text colour 0..1 */
    char     text[PREFBAR_ROWS][PREFBAR_ROW_LEN];
} prefbar_wire_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(prefbar_wire_t) == PREFBAR_WIRE_BYTES, "prefbar_wire_t layout changed");
#else
typedef char prefbar_wire_size_check[(sizeof(prefbar_wire_t) == PREFBAR_WIRE_BYTES) ? 1 : -1];
#endif

/* Returns 1 if the datagram is a well-formed packet. NaN fields fail every
 * range check on purpose. Text rows are NOT checked here: receivers must force
 * NUL-termination and strip control characters themselves. */
static inline int prefbar_wire_valid(const prefbar_wire_t *p, size_t len)
{
    if (!p || len != sizeof(prefbar_wire_t)) return 0;
    if (p->magic != PREFBAR_WIRE_MAGIC || p->version != PREFBAR_WIRE_VERSION) return 0;
    if (p->size != sizeof(prefbar_wire_t) || p->sequence == 0) return 0;
    if (p->rows < 1 || p->rows > PREFBAR_ROWS) return 0;
    if (p->flags & (uint8_t)~PREFBAR_WIRE_KNOWN_FLAGS) return 0;
    if (p->corner != PREFBAR_CORNER_TL && p->corner != PREFBAR_CORNER_BL) return 0;
    if (p->font_size < 10 || p->font_size > 96) return 0;
    if (!(p->margin_x >= 0.0f && p->margin_x <= 1800.0f)) return 0;
    if (!(p->margin_y >= 0.0f && p->margin_y <= 1000.0f)) return 0;
    if (!(p->r >= 0.0f && p->r <= 1.0f && p->g >= 0.0f && p->g <= 1.0f &&
          p->b >= 0.0f && p->b <= 1.0f && p->a >= 0.0f && p->a <= 1.0f)) return 0;
    return 1;
}

#endif /* PREFBAR_WIRE_H */
