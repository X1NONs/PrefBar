/*
 * perfbar-overlay - text-row packet (perfbar payload -> ShellUI renderer)
 * Derived from Common FPS for PS5 / ps5-dualsense-overlay (GPL-3.0-or-later):
 *   Copyright (C) 2026 porhe911, khalifa007 (SimpleFPS), erickdavestech
 * Additions Copyright (C) 2026 the perfbar-overlay authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COMMON_FPS_PERF_WIRE_H
#define COMMON_FPS_PERF_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define PERF_WIRE_MAGIC   0x52414250u   /* bytes 'P','B','A','R' (little endian) */
#define PERF_WIRE_VERSION 1u
#define PERF_WIRE_PORT    39028u
#define PERF_ROWS         6
#define PERF_ROW_LEN      96            /* incl. NUL */
#define PERF_CORNER_TL    0             /* only top-left and bottom-left are  */
#define PERF_CORNER_BL    2             /* implemented; rows are left aligned */
#define PERF_WIRE_BYTES   620u

#pragma pack(push, 1)
typedef struct perf_wire {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                 /* == sizeof(perf_wire_t) */
    uint64_t sequence;             /* non-zero */
    uint8_t  corner;               /* PERF_CORNER_TL / PERF_CORNER_BL */
    uint8_t  rows;                 /* 1..PERF_ROWS used rows */
    uint16_t font_size;            /* px, 10..96 */
    float    margin_x, margin_y;   /* px from the chosen corner */
    float    r, g, b, a;           /* text colour 0..1 */
    char     text[PERF_ROWS][PERF_ROW_LEN];
} perf_wire_t;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(perf_wire_t) == PERF_WIRE_BYTES, "perf_wire_t layout changed");
#else
typedef char perf_wire_size_check[(sizeof(perf_wire_t) == PERF_WIRE_BYTES) ? 1 : -1];
#endif

static inline int perf_wire_valid(const perf_wire_t *p, size_t len)
{
    if (!p || len != sizeof(perf_wire_t)) return 0;
    if (p->magic != PERF_WIRE_MAGIC || p->version != PERF_WIRE_VERSION) return 0;
    if (p->size != sizeof(perf_wire_t) || p->sequence == 0) return 0;
    if (p->rows < 1 || p->rows > PERF_ROWS) return 0;
    if (p->corner != PERF_CORNER_TL && p->corner != PERF_CORNER_BL) return 0;
    if (p->font_size < 10 || p->font_size > 96) return 0;
    if (!(p->margin_x >= 0.0f && p->margin_x <= 1800.0f)) return 0;
    if (!(p->margin_y >= 0.0f && p->margin_y <= 1000.0f)) return 0;
    if (!(p->r >= 0.0f && p->r <= 1.0f && p->g >= 0.0f && p->g <= 1.0f &&
          p->b >= 0.0f && p->b <= 1.0f && p->a >= 0.0f && p->a <= 1.0f)) return 0;
    return 1;
}

#endif /* COMMON_FPS_PERF_WIRE_H */
