/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host self-test for choosing the /dev/dce flip-counter field (issue #17).
 * It includes the telemetry implementation directly to reach the internals.
 *
 *   cc -std=gnu17 -Wall -Wextra -DPREFBAR_SIM -DPREFBAR_EMBED -Iinclude \
 *      scripts/tests/prefbar_dce_selftest.c -o prefbar_dce_selftest -lpthread && ./prefbar_dce_selftest
 */
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
            printf("\n");                        \
        }                                        \
    } while (0)

#define SEC 1000000000ull
#define POLL_NS 2000000ull /* the default poll=2000us */

/* A fake DCE reply: every field rises at its own rate per second. */
typedef struct
{
    double value[DCE_WORDS];
    double rate[DCE_WORDS];
    unsigned cycle[DCE_WORDS]; /* non-zero: field wraps, like a buffer index */
} reply_t;

/* What the sampler loop keeps between polls. */
typedef struct
{
    dce_pick_t pick;
    stats_t st;
    uint64_t t, last_count, report_ns;
    int switches;
} sampler_t;

static void sampler_start(sampler_t *s)
{
    memset(s, 0, sizeof *s);
    dce_pick_reset(&s->pick);
    s->t = 10 * SEC;
    s->report_ns = s->t + SEC;
}

/* Same steps as the game branch of prefbar_run(). */
static void sampler_run(sampler_t *s, reply_t *r, uint64_t duration_ns)
{
    const uint64_t end = s->t + duration_ns;
    for (; s->t < end; s->t += POLL_NS)
    {
        uint64_t words[DCE_WORDS];
        for (int i = 0; i < DCE_WORDS; i++)
        {
            r->value[i] += r->rate[i] * (double)POLL_NS / 1e9;
            words[i] = (uint64_t)r->value[i];
            if (r->cycle[i])
                words[i] %= r->cycle[i];
        }
        if (dce_pick_update(&s->pick, words, s->t))
        {
            s->switches++;
            memset(&s->st, 0, sizeof s->st);
        }
        s->last_count = words[s->pick.word];
        stats_update(&s->st, s->last_count, s->t);
        if (s->t >= s->report_ns)
        {
            s->report_ns = s->t + SEC;
            (void)stats_report(&s->st, s->last_count, s->t);
        }
    }
}

/* Headline numbers over the next second. */
static report_t sampler_report(sampler_t *s, reply_t *r)
{
    (void)stats_report(&s->st, s->last_count, s->t);
    s->report_ns = s->t + 2 * SEC; /* keep sampler_run from reporting */
    sampler_run(s, r, SEC);
    s->report_ns = s->t + SEC;
    return stats_report(&s->st, s->last_count, s->t);
}

static int near(double got, double want, double slack)
{
    return got >= want - slack && got <= want + slack;
}

/* Fields seen in the issue #17 logs and in Common FPS for PS5 on FW 4.51:
 * out + 8 never moves, the flip counter is at out + 0x10. Field 4 stands
 * in for a clock, which rises far too fast to be a frame count. */
static void old_firmware_reply(reply_t *r, double fps)
{
    memset(r, 0, sizeof *r);
    for (int i = 0; i < DCE_WORDS; i++)
        r->value[i] = 1000.0 * (i + 1);
    r->rate[2] = fps;
    r->rate[4] = 1e6;
}

static void test_working_firmware_unchanged(void)
{
    sampler_t s;
    reply_t r;
    sampler_start(&s);
    memset(&r, 0, sizeof r);
    r.value[1] = 5000.0;
    r.rate[1] = 60.0;  /* out + 8 is the counter, as on newer firmware */
    r.rate[2] = 60.0;  /* another field rising at a frame-like rate     */
    r.rate[4] = 1e6;
    sampler_run(&s, &r, 5 * SEC);
    CHECK(s.pick.word == DCE_PREFERRED_WORD && s.switches == 0,
          "a moving out + 8 must be kept (word %d, %d switches)", s.pick.word, s.switches);
    report_t rep = sampler_report(&s, &r);
    CHECK(near(rep.fps, 60.0, 2.0), "60 FPS expected, got %.1f", rep.fps);

    /* Pause or loading screen: out + 8 stops while field 2 keeps rising.
     * Once out + 8 has moved in a game it must stay the source. */
    r.rate[1] = 0.0;
    sampler_run(&s, &r, 10 * SEC);
    CHECK(s.pick.word == DCE_PREFERRED_WORD && s.switches == 0,
          "a pause must not move away from out + 8 (word %d)", s.pick.word);
    rep = sampler_report(&s, &r);
    CHECK(rep.idle && rep.fps == 0.0, "a stopped counter must read idle (fps %.1f)", rep.fps);
}

static void test_old_firmware_counter_found(double fps)
{
    sampler_t s;
    reply_t r;
    sampler_start(&s);
    old_firmware_reply(&r, fps);
    sampler_run(&s, &r, SEC + SEC / 5);
    CHECK(s.pick.word == 2 && s.switches == 1,
          "%.0f FPS: counter at out + 0x10 must be found within ~1 s (word %d, %d switches)",
          fps, s.pick.word, s.switches);
    sampler_run(&s, &r, 3 * SEC);
    report_t rep = sampler_report(&s, &r);
    CHECK(!rep.idle && near(rep.fps, fps, 2.0), "%.0f FPS expected, got %.1f", fps, rep.fps);
    CHECK(near(rep.avg_ms, 1000.0 / fps, 1.0), "%.1f ms frametime expected, got %.2f",
          1000.0 / fps, rep.avg_ms);
    /* polling every 2 ms rounds each frame interval to a 2 ms step */
    CHECK(rep.low1_fps >= 1000.0 / (1000.0 / fps + 2.5) && rep.low1_fps <= fps + 1.0,
          "1%% low near %.0f expected, got %.1f", fps, rep.low1_fps);
    CHECK(s.switches == 1, "steady play must not switch again (%d switches)", s.switches);
}

static void test_old_firmware_pause_and_resume(void)
{
    sampler_t s;
    reply_t r;
    sampler_start(&s);
    old_firmware_reply(&r, 60.0);
    sampler_run(&s, &r, 3 * SEC);
    CHECK(s.pick.word == 2, "counter must be found before the pause (word %d)", s.pick.word);

    r.rate[2] = 0.0; /* game stops presenting */
    sampler_run(&s, &r, SEC + SEC / 10);
    CHECK(s.pick.word == 2, "a short stop must not drop the field (word %d)", s.pick.word);
    sampler_run(&s, &r, SEC);
    CHECK(s.pick.word == DCE_PREFERRED_WORD,
          "three bad windows must drop the field (word %d)", s.pick.word);
    sampler_run(&s, &r, SEC);
    report_t rep = sampler_report(&s, &r);
    CHECK(rep.idle && rep.fps == 0.0, "nothing moving must read idle (fps %.1f)", rep.fps);

    r.rate[2] = 30.0; /* game resumes */
    sampler_run(&s, &r, SEC + SEC / 5);
    CHECK(s.pick.word == 2, "the field must be found again after resuming (word %d)", s.pick.word);
    sampler_run(&s, &r, 2 * SEC);
    rep = sampler_report(&s, &r);
    CHECK(near(rep.fps, 30.0, 2.0), "30 FPS expected after resuming, got %.1f", rep.fps);
}

static void test_no_believable_field(void)
{
    sampler_t s;
    reply_t r;

    /* Static screen on old firmware: only the clock moves. */
    sampler_start(&s);
    old_firmware_reply(&r, 0.0);
    sampler_run(&s, &r, 10 * SEC);
    CHECK(s.pick.word == DCE_PREFERRED_WORD && s.switches == 0,
          "no frame-like field means no switch (word %d)", s.pick.word);

    /* A slow unrelated value at the preferred offset must not hide a real
     * frame counter in another DCE field. */
    sampler_start(&s);
    old_firmware_reply(&r, 60.0);
    r.rate[DCE_PREFERRED_WORD] = 4.0;
    sampler_run(&s, &r, SEC + SEC / 5);
    CHECK(s.pick.word == 2 && s.switches == 1,
          "a 4 Hz preferred-field tick must not mask the 60 FPS counter (word %d)", s.pick.word);
    sampler_run(&s, &r, 3 * SEC);
    report_t preferred_slow_rep = sampler_report(&s, &r);
    CHECK(near(preferred_slow_rep.fps, 60.0, 2.0),
          "60 FPS counter must be used despite a slow preferred-field tick (got %.1f)",
          preferred_slow_rep.fps);

    /* A field whose rate jumps around is not a steady frame counter. */
    sampler_start(&s);
    old_firmware_reply(&r, 20.0);
    for (int i = 0; i < 20; i++)
    {
        r.rate[2] = (i & 1) ? 120.0 : 20.0;
        sampler_run(&s, &r, SEC / 2);
    }
    CHECK(s.pick.word == DCE_PREFERRED_WORD && s.switches == 0,
          "an erratic field must not be chosen (word %d)", s.pick.word);

    /* Too fast to be frames even though it rises steadily. */
    sampler_start(&s);
    old_firmware_reply(&r, 0.0);
    r.rate[3] = 1000.0;
    sampler_run(&s, &r, 5 * SEC);
    CHECK(s.pick.word == DCE_PREFERRED_WORD, "1000/s must not count as FPS (word %d)", s.pick.word);

    /* Too slow: a buffer index cycling 0, 1, 2 with every frame (at 50 FPS
     * it steps up by one per half second), or a 4 Hz tick, rises a little
     * but steadily between two reads. Neither may be taken, not even over
     * the real counter at a higher offset. */
    for (int phase = 0; phase < 3; phase++)
    {
        sampler_start(&s);
        old_firmware_reply(&r, 50.0);
        r.value[0] = phase;
        r.rate[0] = 50.0;
        r.cycle[0] = 3;
        sampler_run(&s, &r, 5 * SEC);
        CHECK(s.pick.word == 2 && s.switches == 1,
              "a cycling index must lose to out + 0x10 (phase %d: word %d, %d switches)",
              phase, s.pick.word, s.switches);
    }
    sampler_start(&s);
    old_firmware_reply(&r, 0.0);
    r.rate[3] = 4.0;
    sampler_run(&s, &r, 5 * SEC);
    CHECK(s.pick.word == DCE_PREFERRED_WORD, "4 per second must not count as FPS (word %d)", s.pick.word);
}

static void test_counter_reset_and_gaps(void)
{
    dce_pick_t p;
    uint64_t w[DCE_WORDS];
    uint64_t t = 50 * SEC;
    dce_pick_reset(&p);
    memset(w, 0, sizeof w);

    /* One good window, then a reading gap longer than DCE_PICK_STALE_NS:
     * the half-built streak must not carry over the gap. */
    CHECK(!dce_pick_update(&p, w, t), "baseline");
    w[2] += 30;
    t += SEC / 2;
    CHECK(!dce_pick_update(&p, w, t), "first good window must not switch yet");
    w[2] += 600;
    t += 10 * SEC;
    CHECK(!dce_pick_update(&p, w, t) && p.word == DCE_PREFERRED_WORD, "gap must restart the search");
    w[2] += 30;
    t += SEC / 2;
    CHECK(!dce_pick_update(&p, w, t), "first window after the gap must not switch");
    w[2] += 30;
    t += SEC / 2;
    CHECK(dce_pick_update(&p, w, t) && p.word == 2, "second window after the gap must switch");

    /* A field that goes backwards is not judged as frames. */
    w[2] -= 5;
    t += SEC / 2;
    CHECK(!dce_pick_update(&p, w, t) && p.word == 2 && p.bad == 1,
          "backwards step counts as one bad window (word %d bad %u)", p.word, p.bad);

    /* New game: back to out + 8 with no history. */
    dce_pick_reset(&p);
    CHECK(p.word == DCE_PREFERRED_WORD && !p.preferred_ok && !p.have_base,
          "reset must start from out + 8");
}

int main(void)
{
    test_working_firmware_unchanged();
    test_old_firmware_counter_found(60.0);
    test_old_firmware_counter_found(30.0);
    test_old_firmware_pause_and_resume();
    test_no_believable_field();
    test_counter_reset_and_gaps();
    printf("prefbar dce selftest: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
