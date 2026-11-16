/* build as C and as C++:  cc -I include scripts/tests/prefbar_wire_selftest.c   /  c++ -x c++ ... */
#include "prefbar/telemetry_wire.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static prefbar_graph_wire_t good_graph(void)
{
    prefbar_graph_wire_t p;
    memset(&p, 0, sizeof p);
    p.magic = PREFBAR_GRAPH_WIRE_MAGIC;
    p.version = PREFBAR_GRAPH_WIRE_VERSION;
    p.size = sizeof p;
    p.sequence = 1;
    p.window_ms = PREFBAR_GRAPH_WINDOW_MS;
    p.scale_max_ms_x10 = PREFBAR_GRAPH_SCALE_MAX_MS_X10;
    p.sample_count = PREFBAR_GRAPH_SAMPLES;
    p.flags = PREFBAR_GRAPH_FLAG_ENABLED;
    p.frametime_x10_ms[0] = 167;
    p.frametime_x10_ms[PREFBAR_GRAPH_SAMPLES - 1] = 333;
    return p;
}

static prefbar_wire_t good(void)
{
    prefbar_wire_t p;
    memset(&p, 0, sizeof p);
    p.magic = PREFBAR_WIRE_MAGIC; p.version = PREFBAR_WIRE_VERSION; p.size = sizeof p;
    p.sequence = 1; p.corner = PREFBAR_CORNER_TL; p.rows = 3;
    p.background_opacity = 75; p.font_size = 24;
    p.margin_x = 20; p.margin_y = 20; p.r = p.g = p.b = p.a = 1.0f;
    strcpy(p.text[0], "FPS 60.0");
    return p;
}

int main(void)
{
    prefbar_wire_t p;
    CHECK(sizeof(prefbar_wire_t) == 1390);
    CHECK(offsetof(prefbar_wire_t, text) == 46);
    p = good(); { unsigned char b[4]; memcpy(b, &p.magic, 4); CHECK(!memcmp(b, "PBAR", 4)); }
    p = good(); CHECK(prefbar_wire_valid(&p, sizeof p) == 1);
    CHECK(offsetof(prefbar_wire_t, text) == 46);
    CHECK(prefbar_wire_valid(NULL, sizeof p) == 0);
    p = good(); CHECK(prefbar_wire_valid(&p, sizeof p - 1) == 0);      /* length mismatch */
    p = good(); p.magic ^= 1;          CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.version = 2;         CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.size = 10;           CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.sequence = 0;        CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.rows = 0;            CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.rows = PREFBAR_ROWS + 1; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_TEXT_SHADOW | PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE | PREFBAR_WIRE_FLAG_OVERLAY_ENABLED | PREFBAR_WIRE_FLAG_HORIZONTAL; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL | PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_SHORT; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL; CHECK(prefbar_wire_valid(&p, sizeof p)); /* medium */
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL | PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_LONG; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL; CHECK(prefbar_wire_valid(&p, sizeof p)); /* centered default */
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL | PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_LEFT; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL | PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_RIGHT; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_LEFT; CHECK(!prefbar_wire_valid(&p, sizeof p)); /* alignment only in horizontal mode */
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL | PREFBAR_WIRE_FLAG_HORIZONTAL_ALIGN_MASK; CHECK(!prefbar_wire_valid(&p, sizeof p)); /* reserved alignment */
    p = good(); p.flags = PREFBAR_WIRE_FLAG_HORIZONTAL_GAP_MASK; CHECK(!prefbar_wire_valid(&p, sizeof p)); /* reserved 11 */
    p = good(); p.flags = 0x80; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.rows = PREFBAR_ROWS;    CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.corner = PREFBAR_CORNER_TR; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.corner = PREFBAR_CORNER_BL; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.corner = PREFBAR_CORNER_BR; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.corner = PREFBAR_CORNER_BR + 1; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.background_opacity = 0; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.background_opacity = 50; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.background_opacity = 100; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.background_opacity = 49; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.background_opacity = 101; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.font_size = 9;       CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.font_size = 97;      CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.margin_x = NAN;      CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.margin_y = -1.0f;    CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.margin_x = INFINITY; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.r = 1.5f;            CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.a = NAN;             CHECK(!prefbar_wire_valid(&p, sizeof p));
    prefbar_graph_wire_t gp;
    CHECK(sizeof(prefbar_graph_wire_t) == PREFBAR_GRAPH_WIRE_BYTES);
    CHECK(sizeof(prefbar_graph_wire_t) == 120);
    CHECK(offsetof(prefbar_graph_wire_t, frametime_x10_ms) == 24);
    gp = good_graph();
    { unsigned char b[4]; memcpy(b, &gp.magic, 4); CHECK(!memcmp(b, "PBRG", 4)); }
    CHECK(prefbar_graph_wire_valid(&gp, sizeof gp) == 1);
    CHECK(prefbar_graph_wire_valid(NULL, sizeof gp) == 0);
    CHECK(prefbar_graph_wire_valid(&gp, sizeof gp - 1) == 0);
    gp = good_graph(); gp.magic ^= 1; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.version++; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.size--; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.sequence = 0; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.window_ms++; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.sample_count--; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.reserved = 1; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.flags = 0x80; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.frametime_x10_ms[3] = PREFBAR_GRAPH_SCALE_MAX_MS_X10 + 1;
    CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); gp.flags = 0; CHECK(!prefbar_graph_wire_valid(&gp, sizeof gp));
    gp = good_graph(); memset(gp.frametime_x10_ms, 0, sizeof gp.frametime_x10_ms);
    gp.flags = 0; CHECK(prefbar_graph_wire_valid(&gp, sizeof gp));

    printf(fails ? "PrefBar wire selftest: %d FAILED\n" : "PrefBar wire selftest: all passed\n", fails);
    return fails != 0;
}
