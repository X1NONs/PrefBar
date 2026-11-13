/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/ps5/telemetry/prefbar_telemetry.c"

static int g_fail;
#define CHECK(cond, ...)                         \
    do                                           \
    {                                            \
        if (!(cond))                             \
        {                                        \
            g_fail++;                            \
            printf("FAIL  line %d: ", __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                       \
        }                                        \
    } while (0)

static void force(int rc, int total, int fr)
{
    g_sim_pool_force = 1;
    g_sim_pool_rc = rc;
    g_sim_pool_total = total;
    g_sim_pool_fr = fr;
}

int main(void)
{
    pool_hold_t h;
    int t = 0, f = 0;

    CHECK(pool_sane(6144, 4096), "normal read must be sane");
    CHECK(pool_sane(6144, 0), "fully used pool is legal");
    CHECK(pool_sane(6144, 6144), "fully free pool is legal");
    CHECK(!pool_sane(0, 0), "zero total is not sane");
    CHECK(!pool_sane(-1, 0), "negative total is not sane");
    CHECK(!pool_sane(65535, 1000), "65535 MB total must be rejected");
    CHECK(!pool_sane(66000, 1000), "oversized total must be rejected");
    CHECK(!pool_sane(6144, 7000), "free greater than total must be rejected");
    CHECK(!pool_sane(6144, -59000), "negative free must be rejected");

    memset(&h, 0, sizeof h);
    force(0, 6144, 4096);
    CHECK(pool_read(1, &h, "t.reject", &t, &f) && t == 6144 && f == 4096,
          "valid reading must be accepted");

    force(0, 66000, 1000);
    for (int i = 0; i < POOL_HOLD_TICKS; i++)
    {
        t = f = 0;
        CHECK(pool_read(1, &h, "t.reject", &t, &f) && t == 6144 && f == 4096,
              "bad read %d must hold the last valid value (got %d/%d)", i + 1, t, f);
    }
    CHECK(!pool_read(1, &h, "t.reject", &t, &f),
          "row must hide after %d bad reads", POOL_HOLD_TICKS);
    CHECK(!pool_read(1, &h, "t.reject", &t, &f),
          "row must stay hidden while reads remain invalid");

    force(0, 6144, 3000);
    CHECK(pool_read(1, &h, "t.reject", &t, &f) && t == 6144 && f == 3000,
          "first valid read must recover");
    CHECK(h.bad_run == 0, "bad-read count must reset on recovery");

    force(-1, 0, 0);
    CHECK(pool_read(1, &h, "t.reject", &t, &f) && t == 6144,
          "API failure must briefly hold the last valid value");

    memset(&h, 0, sizeof h);
    force(0, 66000, 1000);
    CHECK(!pool_read(1, &h, "t.reject", &t, &f),
          "invalid first read must be hidden");

    memset(&g_pool_ram, 0, sizeof g_pool_ram);
    memset(&g_pool_vram, 0, sizeof g_pool_vram);
    cpuu_t c;
    sensors_t s;
    memset(&c, 0, sizeof c);
    force(0, 66000, 1000);
    sensors_read(&s, &c, now_ns());
    CHECK(!s.have_ram && !s.have_vram,
          "invalid pools must not produce RAM/VRAM readings");

    force(0, 6144, 5000);
    sensors_read(&s, &c, now_ns());
    CHECK(s.have_ram && s.ram_used_mb == 1144 && s.ram_total_mb == 6144,
          "valid reading must show 1144/6144 MB (got %d/%d)",
          s.ram_used_mb, s.ram_total_mb);

    force(0, 66000, 1000);
    sensors_read(&s, &c, now_ns());
    CHECK(s.have_ram && s.ram_used_mb == 1144 && s.ram_used_mb <= POOL_MAX_MB,
          "invalid reading must retain the last valid RAM value");

    g_sim_pool_force = 0;
    printf("prefbar pool selftest: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
