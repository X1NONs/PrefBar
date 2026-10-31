/* build as C and as C++:  cc -I include scripts/tests/prefbar_wire_selftest.c   /  c++ -x c++ ... */
#include "prefbar/telemetry_wire.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static prefbar_wire_t good(void)
{
    prefbar_wire_t p;
    memset(&p, 0, sizeof p);
    p.magic = PREFBAR_WIRE_MAGIC; p.version = PREFBAR_WIRE_VERSION; p.size = sizeof p;
    p.sequence = 1; p.corner = PREFBAR_CORNER_TL; p.rows = 3; p.font_size = 24;
    p.margin_x = 20; p.margin_y = 20; p.r = p.g = p.b = p.a = 1.0f;
    strcpy(p.text[0], "FPS 60.0");
    return p;
}

int main(void)
{
    prefbar_wire_t p;
    CHECK(sizeof(prefbar_wire_t) == 621);
    CHECK(offsetof(prefbar_wire_t, text) == 45);
    p = good(); { unsigned char b[4]; memcpy(b, &p.magic, 4); CHECK(!memcmp(b, "PBAR", 4)); }
    p = good(); CHECK(prefbar_wire_valid(&p, sizeof p) == 1);
    CHECK(prefbar_wire_valid(NULL, sizeof p) == 0);
    p = good(); CHECK(prefbar_wire_valid(&p, sizeof p - 1) == 0);      /* length mismatch */
    p = good(); p.magic ^= 1;          CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.version = 1;         CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.size = 10;           CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.sequence = 0;        CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.rows = 0;            CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.rows = PREFBAR_ROWS + 1; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = PREFBAR_WIRE_FLAG_TEXT_SHADOW | PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE | PREFBAR_WIRE_FLAG_OVERLAY_ENABLED; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.flags = 0x80; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.rows = PREFBAR_ROWS;    CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.corner = 1;          CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.corner = PREFBAR_CORNER_BL; CHECK(prefbar_wire_valid(&p, sizeof p));
    p = good(); p.font_size = 9;       CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.font_size = 97;      CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.margin_x = NAN;      CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.margin_y = -1.0f;    CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.margin_x = INFINITY; CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.r = 1.5f;            CHECK(!prefbar_wire_valid(&p, sizeof p));
    p = good(); p.a = NAN;             CHECK(!prefbar_wire_valid(&p, sizeof p));
    printf(fails ? "PrefBar wire selftest: %d FAILED\n" : "PrefBar wire selftest: all passed\n", fails);
    return fails != 0;
}
