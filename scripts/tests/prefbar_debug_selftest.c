/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host self-test for the prefbar debug-event layer (cooldown, toast cap, level gating,
 * log rotation, thread safety). It includes the telemetry implementation directly to reach the internals.
 *
 *   cc -std=gnu17 -Wall -Wextra -DPREFBAR_SIM -DPREFBAR_EMBED -Iinclude \
 *      scripts/tests/prefbar_debug_selftest.c -o prefbar_debug_selftest -lpthread && ./prefbar_debug_selftest
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

static void reset(const char *conf)
{
    (void)!system("rm -rf /tmp/prefbar");
    mkdir(DATA_DIR, 0777);
    if (conf)
    {
        FILE *f = fopen(DATA_DIR "/prefbar.conf", "w");
        if (f)
        {
            fputs(conf, f);
            fclose(f);
        }
    }
    memset(&g_dbg, 0, sizeof g_dbg);
    g_sim_toasts = 0;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void age_all_slots(void) /* pretend 60 s passed */
{
    for (int i = 0; i < g_dbg.nslots; i++)
        if (g_dbg.slot[i].last_ns)
            g_dbg.slot[i].last_ns -= 60ull * 1000000000ull;
}

static void test_cooldown(void)
{
    reset(NULL);
    for (int i = 0; i < 1000; i++)
        prefbar_debug_event(DBG_ERR, "k", "boom %d", i);
    CHECK(g_sim_toasts == 1, "same key x1000 in cooldown: expected 1 toast, got %d", g_sim_toasts);
    age_all_slots();
    prefbar_debug_event(DBG_ERR, "k", "boom again");
    CHECK(g_sim_toasts == 2, "after cooldown: expected 2 toasts, got %d", g_sim_toasts);
    FILE *f = fopen(DATA_DIR "/debug.log", "r");
    char line[400], last[400] = "";
    int n = 0;
    while (f && fgets(line, sizeof line, f))
    {
        snprintf(last, sizeof last, "%s", line);
        n++;
    }
    if (f)
        fclose(f);
    CHECK(n == 2, "debug.log should hold 2 lines (suppressed events are not logged), has %d", n);
    CHECK(strstr(last, "(+999 more)") != NULL, "second line should report 999 suppressed: %s", last);
}

static void test_levels(void)
{
    static const struct
    {
        const char *conf;
        int expect;
    } cases[] = {
        {"debug_notify=0\n", 0},
        {"debug_notify=1\n", 1},
        {"debug_notify=2\n", 2},
        {"debug_notify=3\n", 3},
        {"debug_notify=4\n", 4},
        {NULL, 3}, /* default */
        {"debug_notify=9\n", 3},
        {"debug_notify=-1\n", 3},
        {"debug_notify=abc\n", 3},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++)
    {
        reset(cases[c].conf);
        prefbar_debug_event(DBG_ERR, "e", "e");
        prefbar_debug_event(DBG_WARN, "w", "w");
        prefbar_debug_event(DBG_INFO, "i", "i");
        prefbar_debug_event(DBG_TRACE, "t", "t");
        CHECK(g_sim_toasts == cases[c].expect, "conf=[%s]: expected %d toasts, got %d",
              cases[c].conf ? cases[c].conf : "(none)", cases[c].expect, g_sim_toasts);
        long n = 0;
        FILE *f = fopen(DATA_DIR "/debug.log", "r");
        char b[300];
        while (f && fgets(b, sizeof b, f))
            n++;
        if (f)
            fclose(f);
        CHECK(n == 4, "conf=[%s]: log must record all 4 events regardless of toast level, has %ld",
              cases[c].conf ? cases[c].conf : "(none)", n);
    }
}

static void test_toast_cap(void)
{
    reset("debug_notify=4\n");
    char key[16];
    for (int i = 0; i < 40; i++)
    {
        snprintf(key, sizeof key, "key%d", i);
        prefbar_debug_event(DBG_ERR, key, "event %d", i);
    }
    CHECK(g_sim_toasts == DBG_MAX_TOASTS, "toast cap: expected %d, got %d", DBG_MAX_TOASTS, g_sim_toasts);
    for (int i = 40; i < 200; i++)
    {
        snprintf(key, sizeof key, "key%d", i);
        prefbar_debug_event(DBG_ERR, key, "event %d", i);
    }
    CHECK(g_sim_toasts == DBG_MAX_TOASTS, "cap must hold after 200 distinct keys, got %d", g_sim_toasts);
    int before = g_dbg.nslots;
    prefbar_debug_event(DBG_INFO, "late-info-key", "x");
    CHECK(g_dbg.nslots == before && before == DBG_MAX_KEYS, "key table must stay bounded (%d)", g_dbg.nslots);
}

static void test_rotation(void)
{
    reset("debug_notify=0\n");
    for (int i = 0; i < 2000; i++)
    {
        prefbar_debug_event(DBG_INFO, "rot", "padding padding padding padding padding padding event number %d", i);
        age_all_slots();
    }
    long a = file_size(DATA_DIR "/debug.log"), b = file_size(DATA_DIR "/debug.log.1");
    CHECK(b > 0, "debug.log.1 should exist after >64 KB of events");
    CHECK(a > 0 && a <= DBG_LOG_MAX + 512, "debug.log must stay bounded, is %ld bytes", a);
}

static void test_guards(void)
{
    reset(NULL);
    prefbar_debug_event(DBG_INFO, NULL, "x");
    prefbar_debug_event(DBG_INFO, "k", NULL);
    prefbar_debug_event(-5, "lo", "clamped low");
    prefbar_debug_event(99, "hi", "clamped high");
    CHECK(g_sim_toasts == 1, "NULL args ignored, level -5 -> ERR toast, 99 -> TRACE no toast; got %d", g_sim_toasts);
    char big[2000];
    memset(big, 'A', sizeof big - 1);
    big[sizeof big - 1] = 0;
    prefbar_debug_event(DBG_ERR, "long", "%s", big); /* must truncate, not overflow */
    CHECK(1, "unreachable");
}

static void test_notification_switches(void)
{
    reset("notifications=0\ndebug_notify=4\n");
    prefbar_debug_event(DBG_ERR, "disabled.error", "error");
    prefbar_debug_event(DBG_INFO, "disabled.info", "info");
    CHECK(g_sim_toasts == 0, "notifications=0 must suppress every toast, got %d", g_sim_toasts);

    reset("notifications=1\nerror_notifications=0\ndebug_notifications=1\ndebug_notify=4\n");
    prefbar_debug_event(DBG_ERR, "muted.error", "error");
    prefbar_debug_event(DBG_INFO, "allowed.info", "info");
    CHECK(g_sim_toasts == 1, "error_notifications=0 should suppress only error toasts, got %d", g_sim_toasts);

    reset("notifications=1\nerror_notifications=1\ndebug_notifications=0\ndebug_notify=4\n");
    prefbar_debug_event(DBG_ERR, "allowed.error", "error");
    prefbar_debug_event(DBG_INFO, "muted.info", "info");
    CHECK(g_sim_toasts == 1, "debug_notifications=0 should preserve error toasts only, got %d", g_sim_toasts);
}

static void test_config_file_and_values(void)
{
    reset(NULL);
    ensure_default_config_file();
    CHECK(access(PREFBAR_CONFIG_PATH, F_OK) == 0,
          "default prefbar.conf should be created on first start");
    config_t cfg;
    (void)load_config(&cfg);
    CHECK(cfg.overlay == 1 && cfg.text_shadow == 1,
          "default overlay and shadow should be enabled");
    CHECK(cfg.show_fps == 1 && cfg.show_cpu == 1 && cfg.show_soc_temp == 1,
          "default telemetry rows should be enabled");
    CHECK(cfg.notifications == 1 && cfg.touchpad_toggle == 1,
          "default notifications and touchpad toggle should be enabled");

    FILE *f = fopen(PREFBAR_CONFIG_PATH, "w");
    CHECK(f != NULL, "open custom config");
    if (f)
    {
        fputs("# custom config\n[overlay]\noverlay=0\ntext_color=#123456\ntext_shadow=0\noverlay_font=32\noverlay_corner=2\noverlay_x=44\noverlay_y=55\n"
              "[notifications]\nnotifications=0\nstartup_notification=0\nerror_notifications=0\ndebug_notifications=0\ndebug_notify=0\n"
              "[telemetry]\nshow_fps=0\nshow_cpu=1\nshow_soc_temp=0\nshow_fan_duty=0\nshow_memory=1\n"
              "[input]\ntouchpad_toggle=0\n",
              f);
        fclose(f);
    }
    (void)load_config(&cfg);
    CHECK(cfg.overlay == 0 && cfg.text_shadow == 0,
          "overlay and text shadow settings should parse");
    CHECK(cfg.ov_font == 32 && cfg.ov_corner == PREFBAR_CORNER_BL &&
              cfg.ov_x == 44 && cfg.ov_y == 55,
          "overlay geometry should parse");
    CHECK(cfg.text_r > 0.07f && cfg.text_r < 0.08f &&
              cfg.text_g > 0.20f && cfg.text_g < 0.21f &&
              cfg.text_b > 0.33f && cfg.text_b < 0.34f,
          "#123456 should parse into normalized RGB values");
    CHECK(cfg.notifications == 0 && cfg.startup_notification == 0 &&
              cfg.error_notifications == 0 && cfg.debug_notifications == 0,
          "notification switches should parse");
    CHECK(cfg.show_fps == 0 && cfg.show_cpu == 1 && cfg.show_soc_temp == 0 &&
              cfg.show_fan_duty == 0 && cfg.show_memory == 1 && cfg.touchpad_toggle == 0,
          "telemetry and input switches should parse");
}

static void *hammer(void *arg)
{
    int id = (int)(intptr_t)arg;
    char key[16];
    for (int i = 0; i < 3000; i++)
    {
        snprintf(key, sizeof key, "t%d.%d", id, i % 7);
        prefbar_debug_event(1 + (i % 4), key, "thread %d event %d", id, i);
        if (i % 500 == 0)
            age_all_slots(); /* racy by design: only moves timestamps back */
    }
    return NULL;
}

static void test_threads(void)
{
    reset("debug_notify=4\n");
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, hammer, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    CHECK(g_sim_toasts <= DBG_MAX_TOASTS, "4 threads: toast cap violated (%d)", g_sim_toasts);
    CHECK(g_sim_toasts > 0, "4 threads: expected some toasts");
    CHECK(g_dbg.nslots <= DBG_MAX_KEYS, "4 threads: key table overflow (%d)", g_dbg.nslots);
    long a = file_size(DATA_DIR "/debug.log");
    CHECK(a >= 0 && a <= DBG_LOG_MAX + 4096, "4 threads: log unbounded (%ld)", a);
}

static void test_single_instance(void)
{
    reset("debug_notify=0\n");
    int lock = prefbar_acquire_lock();
    CHECK(lock >= 0, "first instance must acquire the lock");
    if (lock < 0)
        return;
    CHECK(prefbar_telemetry_embedded_start() != 0,
          "duplicate embedded startup must fail synchronously");
    close(lock);
    lock = prefbar_acquire_lock();
    CHECK(lock >= 0, "lock must be available after its owner releases it");
    if (lock >= 0)
        close(lock);
}

int main(void)
{
    (void)!freopen("/dev/null", "w", stderr); /* silence simulated toasts */
    test_cooldown();
    test_levels();
    test_notification_switches();
    test_config_file_and_values();
    test_toast_cap();
    test_rotation();
    test_guards();
    test_threads();
    test_single_instance();
    printf(g_fail ? "prefbar debug selftest: %d FAILED\n" : "prefbar debug selftest: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
