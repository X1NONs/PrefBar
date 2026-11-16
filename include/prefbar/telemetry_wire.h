/*
 * PrefBar - text-row packet (PrefBar telemetry -> ShellUI renderer)
 *
 * Derived from Common FPS for PS5 / ps5-dualsense-overlay (GPL-3.0-or-later):
 *   Copyright (C) 2026 porhe911, khalifa007 (SimpleFPS), erickdavestech
 * Additions Copyright (C) 2026 the prefbar authors.
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
#define PREFBAR_WIRE_VERSION 6u
#define PREFBAR_WIRE_PORT    39028u
#define PREFBAR_CONTROL_PORT     39029u   /* ShellUI -> controller debug/control events */
#define PREFBAR_ROWS         14
#define PREFBAR_ROW_LEN      96            /* incl. NUL */
#define PREFBAR_CORNER_TL    0             /* top-left */
#define PREFBAR_CORNER_TR    1             /* top-right */
#define PREFBAR_CORNER_BL    2             /* bottom-left */
#define PREFBAR_CORNER_BR    3             /* bottom-right */
#define PREFBAR_WIRE_BYTES   1390u
/* Optional high-frequency frame-time graph packet; v6 text packet is unchanged. */
#define PREFBAR_GRAPH_WIRE_MAGIC   0x47524250u /* little-endian "PBRG" */
#define PREFBAR_GRAPH_WIRE_VERSION 1u
#define PREFBAR_GRAPH_SAMPLES      48u
#define PREFBAR_GRAPH_WINDOW_MS    3000u
#define PREFBAR_GRAPH_SCALE_MAX_MS_X10 500u /* 50.0 ms in 0.1 ms units */
#define PREFBAR_GRAPH_FLAG_ENABLED 0x01u
#define PREFBAR_GRAPH_KNOWN_FLAGS  PREFBAR_GRAPH_FLAG_ENABLED
#define PREFBAR_GRAPH_WIRE_BYTES   (24u + PREFBAR_GRAPH_SAMPLES * 2u)
#define PREFBAR_WIRE_FLAG_TEXT_SHADOW       0x01u
#define PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE   0x02u
#define PREFBAR_WIRE_FLAG_OVERLAY_ENABLED   0x04u
#define PREFBAR_WIRE_FLAG_HORIZONTAL        0x08u
/* Horizontal spacing: 00 = medium, 01 = short, 10 = long, 11 = invalid. */
#define PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_MASK  0x30u
#define PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_SHORT 0x10u
#define PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_LONG  0x20u
/* Horizontal alignment: 00 = center, 01 = left, 10 = right, 11 = invalid. */
#define PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_MASK  0xC0u
#define PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_LEFT  0x40u
#define PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_RIGHT 0x80u
#define PREFBAR_WIRE_KNOWN_FLAGS (PREFBAR_WIRE_FLAG_TEXT_SHADOW | PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE | PREFBAR_WIRE_FLAG_OVERLAY_ENABLED | PREFBAR_WIRE_FLAG_HORIZONTAL | PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_MASK | PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_MASK)

#pragma pack(push, 1)
typedef struct prefbar_wire {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                 /* == sizeof(prefbar_wire_t) */
    uint64_t sequence;             /* non-zero */
    uint8_t  corner;               /* PREFBAR_CORNER_TL / TR / BL / BR */
    uint8_t  rows;                 /* 1..PREFBAR_ROWS used rows */
    uint8_t  flags;                /* PREFBAR_WIRE_FLAG_* */
    uint8_t  background_opacity;   /* 0 = disabled, 50..100 = black background alpha (%) */
    uint16_t font_size;            /* px, 10..96 */
    float    margin_x, margin_y;   /* px from the chosen corner */
    float    r, g, b, a;           /* text colour 0..1 */
    char     text[PREFBAR_ROWS][PREFBAR_ROW_LEN];
} prefbar_wire_t;

/* 48 chronological buckets across 3 seconds; each stores the maximum
 * observed frame interval in 0.1 ms units. */
typedef struct prefbar_graph_wire {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t sequence;
    uint16_t window_ms;
    uint16_t scale_max_ms_x10;
    uint8_t  sample_count;
    uint8_t  flags;
    uint16_t reserved;
    uint16_t frametime_x10_ms[PREFBAR_GRAPH_SAMPLES];
} prefbar_graph_wire_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(prefbar_wire_t) == PREFBAR_WIRE_BYTES, "prefbar_wire_t layout changed");
static_assert(sizeof(prefbar_graph_wire_t) == PREFBAR_GRAPH_WIRE_BYTES, "prefbar_graph_wire_t layout changed");
#else
typedef char prefbar_wire_size_check[(sizeof(prefbar_wire_t) == PREFBAR_WIRE_BYTES) ? 1 : -1];
typedef char prefbar_graph_wire_size_check[(sizeof(prefbar_graph_wire_t) == PREFBAR_GRAPH_WIRE_BYTES) ? 1 : -1];
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
    if ((p->flags & PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_MASK) ==
        PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_MASK) return 0;
    if ((p->flags & PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_MASK) ==
        PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_MASK) return 0;
    if (!(p->flags & PREFBAR_WIRE_FLAG_HORIZONTAL) &&
        (p->flags & (PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_MASK |
                     PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_MASK))) return 0;
    if (p->corner > PREFBAR_CORNER_BR) return 0;
    if (p->background_opacity != 0 &&
        (p->background_opacity < 50 || p->background_opacity > 100)) return 0;
    if (p->font_size < 10 || p->font_size > 96) return 0;
    if (!(p->margin_x >= 0.0f && p->margin_x <= 1800.0f)) return 0;
    if (!(p->margin_y >= 0.0f && p->margin_y <= 1000.0f)) return 0;
    if (!(p->r >= 0.0f && p->r <= 1.0f && p->g >= 0.0f && p->g <= 1.0f &&
          p->b >= 0.0f && p->b <= 1.0f && p->a >= 0.0f && p->a <= 1.0f)) return 0;
    return 1;
}

/* Strict validator: fixed geometry keeps rendering deterministic. */
static inline int prefbar_graph_wire_valid(const prefbar_graph_wire_t *p, size_t len)
{
    if (!p || len != sizeof(prefbar_graph_wire_t)) return 0;
    if (p->magic != PREFBAR_GRAPH_WIRE_MAGIC ||
        p->version != PREFBAR_GRAPH_WIRE_VERSION ||
        p->size != sizeof(prefbar_graph_wire_t) || p->sequence == 0) return 0;
    if (p->window_ms != PREFBAR_GRAPH_WINDOW_MS ||
        p->scale_max_ms_x10 != PREFBAR_GRAPH_SCALE_MAX_MS_X10 ||
        p->sample_count != PREFBAR_GRAPH_SAMPLES || p->reserved != 0) return 0;
    if (p->flags & (uint8_t)~PREFBAR_GRAPH_KNOWN_FLAGS) return 0;
    for (size_t i = 0; i < PREFBAR_GRAPH_SAMPLES; ++i) {
        if (p->frametime_x10_ms[i] > p->scale_max_ms_x10) return 0;
        if (!(p->flags & PREFBAR_GRAPH_FLAG_ENABLED) && p->frametime_x10_ms[i] != 0) return 0;
    }
    return 1;
}

#endif /* PREFBAR_WIRE_H */
