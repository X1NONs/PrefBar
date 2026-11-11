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
    CHECK(cfg.show_power == 1 && cfg.show_cpu_clock == 1 &&
              cfg.show_gpu_clock == 1 && cfg.overlay_horizontal == 0,
          "new power, clock, and horizontal-layout options should have their defaults");

    FILE *defaults_file = fopen(PREFBAR_CONFIG_PATH, "r");
    char defaults_text[4096] = {0};
    if (defaults_file)
    {
        (void)fread(defaults_text, 1, sizeof(defaults_text) - 1, defaults_file);
        fclose(defaults_file);
    }
    char *overlay_section = strstr(defaults_text, "[overlay]");
    char *telemetry_section = strstr(defaults_text, "[telemetry]");
    char *horizontal_option = strstr(defaults_text, "overlay_horizontal=0");
    CHECK(overlay_section && telemetry_section && horizontal_option &&
              overlay_section < horizontal_option && horizontal_option < telemetry_section,
          "overlay_horizontal should be grouped under [overlay]");
    CHECK(cfg.show_fps == 1 && cfg.show_cpu == 1 && cfg.show_soc_temp == 1 &&
              cfg.show_cpu_clock == 1 && cfg.show_gpu_clock == 1,
          "default telemetry rows and clocks should be enabled");
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
    ensure_default_config_file(); /* upgrade migration appends only missing v0.2.1 options */
    (void)load_config(&cfg);
    CHECK(cfg.overlay == 0 && cfg.show_fps == 0,
          "config migration should preserve existing user settings");
    CHECK(cfg.show_power == 1 && cfg.overlay_horizontal == 0,
          "config migration should add missing options with their defaults");

    f = fopen(PREFBAR_CONFIG_PATH, "w");
    CHECK(f != NULL, "open config with explicit new options");
    if (f)
    {
        fputs("[overlay]\noverlay=0\ntext_color=#123456\ntext_shadow=0\noverlay_font=32\noverlay_corner=2\noverlay_x=44\noverlay_y=55\noverlay_horizontal=1\n"
              "[notifications]\nnotifications=0\nstartup_notification=0\nerror_notifications=0\ndebug_notifications=0\ndebug_notify=0\n"
              "[telemetry]\nshow_fps=0\nshow_cpu=1\nshow_soc_temp=0\nshow_fan_duty=0\nshow_memory=1\nshow_power=0\nshow_cpu_clock=0\nshow_gpu_clock=0\n"
              "[input]\ntouchpad_toggle=0\n", f);
        fclose(f);
    }
    ensure_default_config_file();
    (void)load_config(&cfg);
    CHECK(cfg.show_power == 0 && cfg.overlay_horizontal == 1,
          "config migration should preserve explicit user values");
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
              cfg.show_cpu_clock == 0 && cfg.show_gpu_clock == 0 &&
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

static void test_compact_overlay_rows(void)
{
    config_t cfg = {0};
    report_t report = {0};
    sensors_t sens = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];

    cfg.sensors = 1;
    cfg.show_fps = 1;
    cfg.show_cpu = 1;
    cfg.show_soc_temp = 1;
    cfg.show_fan_duty = 0;
    cfg.show_memory = 1;
    cfg.show_power = 0;
    report.fps = 60.0;
    report.avg_ms = 16.7;
    report.low1_fps = 58.0;
    sens.have_cpu = 1;
    sens.cpu_avg = 42.0f;
    sens.have_gpu_temp = 1;
    sens.gpu_temp = 55;
    sens.have_cpu_clock = 1;
    sens.cpu_clock_mhz = 3500;
    sens.have_gpu_clock = 1;
    sens.gpu_clock_mhz = 2150;
    cfg.show_cpu_clock = 1;
    cfg.show_gpu_clock = 1;
    sens.have_ram = 1;
    sens.ram_used_mb = 2500;
    sens.ram_total_mb = 6144;

    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 3, "disabled fan row should compact; expected 3 rows, got %d", n);
    CHECK(strncmp(rows[0], "FPS ", 4) == 0, "FPS should remain row 0: %s", rows[0]);
    CHECK(strncmp(rows[1], "CPU ", 4) == 0 &&
              strstr(rows[1], "3500MHz") != NULL &&
              strstr(rows[1], "GPU 2150MHz") != NULL &&
              strstr(rows[1], "55C") != NULL,
          "CPU/GPU clocks and GPU temperature should share row 1: %s", rows[1]);
    CHECK(strncmp(rows[2], "RAM ", 4) == 0,
          "RAM should move into row 2 when fan is disabled: %s", rows[2]);

    cfg.show_fps = 0;
    cfg.show_cpu = 0;
    cfg.show_soc_temp = 0;
    cfg.show_cpu_clock = 0;
    cfg.show_gpu_clock = 0;
    cfg.show_fan_duty = 1;
    sens.have_fan = 1;
    sens.fan = 23;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 2, "disabled FPS/CPU should compact; expected 2 rows, got %d", n);
    CHECK(strncmp(rows[0], "Fan: ", 5) == 0, "fan should become row 0: %s", rows[0]);
    CHECK(strncmp(rows[1], "RAM ", 4) == 0, "RAM should become row 1: %s", rows[1]);

    cfg.ov_mode = 1;
    cfg.show_cpu = 1;
    cfg.show_fan_duty = 0;
    sens.have_cpu = 1;
    sens.dbg_have = 1;
    for (int i = 0; i < NCORES; ++i) {
        sens.core_ok[i] = 1;
        sens.core[i] = 25.0f + (float)i;
        sens.dbg_ratio[i] = 0.5f;
    }
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 4, "advanced rows should append only populated rows; expected 4, got %d", n);
    CHECK(strncmp(rows[2], "cores:", 6) == 0, "core row should follow active metrics: %s", rows[2]);
    CHECK(strncmp(rows[3], "idle/dt:", 8) == 0, "diagnostic row should follow core row: %s", rows[3]);

    cfg.sensors = 0;
    cfg.show_fps = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && rows[0][0] == '\0',
          "all-disabled overlay must send one empty clearing row (n=%d, text='%s')", n, rows[0]);
}

static void test_clock_sensor_and_row(void)
{
    reset("show_cpu_clock=1\nshow_gpu_clock=1\n");
    config_t cfg;
    CHECK(load_config(&cfg) == 1, "clock config should load");
    CHECK(cfg.show_cpu_clock == 1 && cfg.show_gpu_clock == 1,
          "CPU/GPU clock settings should parse as enabled");

    sensors_t sens;
    cpuu_t cpuu = {0};
    sensors_read(&sens, &cpuu, now_ns());
    CHECK(sens.have_cpu_clock && sens.cpu_clock_mhz == 3500,
          "simulated CPU frequency should convert to 3500 MHz");
    CHECK(sens.have_gpu_clock && sens.gpu_clock_mhz == 2150,
          "SoC clock domain 20 should provide GPU GFXCLK in MHz");
    CHECK(sens.have_gpu_temp && sens.gpu_temp >= 55,
          "SoC sensor 0 should be exposed as GPU temperature");

    cfg.sensors = 1;
    cfg.show_fps = 0;
    cfg.show_cpu = 1;
    cfg.show_soc_temp = 1;
    cfg.show_cpu_clock = 1;
    cfg.show_gpu_clock = 1;
    cfg.show_fan_duty = 0;
    cfg.show_memory = 0;
    cfg.show_power = 0;
    report_t report = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];
    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && strstr(rows[0], "3500MHz") &&
              strstr(rows[0], "GPU 2150MHz") && strstr(rows[0], "C"),
          "clocks and GPU temperature should share one compact row (n=%d, row='%s')",
          n, rows[0]);

    char kv[1024];
    format_kv(&report, &sens, kv, sizeof kv);
    CHECK(strstr(kv, "cpu_clock_mhz=3500\n") &&
              strstr(kv, "gpu_clock_mhz=2150\n") &&
              strstr(kv, "gpu_temp_c=") &&
              strstr(kv, "soc_temp_c="),
          "status.kv should expose new keys and retain the legacy temperature key");
}

static void test_power_sensor_and_row(void)
{
    reset("show_power=1\noverlay_horizontal=1\n");
    config_t cfg;
    CHECK(load_config(&cfg) == 1, "power config should load");
    CHECK(cfg.show_power == 1, "show_power should be enabled by config");
    CHECK(cfg.overlay_horizontal == 1, "overlay_horizontal should parse as enabled");

    sensors_t sens;
    cpuu_t cpuu = {0};
    sensors_read(&sens, &cpuu, now_ns());
    CHECK(sens.have_power == 1, "simulated SoC power API should provide a sample");
    CHECK(sens.power_w > 100.0f && sens.power_w < 300.0f,
          "power should be converted from milliwatts to watts (got %.2f W)", sens.power_w);

    cfg.sensors = 1;
    cfg.show_fps = 0;
    cfg.show_cpu = 0;
    cfg.show_soc_temp = 0;
    cfg.show_cpu_clock = 0;
    cfg.show_gpu_clock = 0;
    cfg.show_fan_duty = 0;
    cfg.show_memory = 0;
    cfg.show_power = 1;
    report_t report = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];
    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && strncmp(rows[0], "PWR ", 4) == 0 && strstr(rows[0], " W") != NULL,
          "PWR should be emitted as a compact standalone row (n=%d, row='%s')", n, rows[0]);

    cfg.show_power = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && rows[0][0] == '\0', "disabled PWR should not leave a row");

    cfg.show_power = 1;
    cfg.show_fps = 1;
    cfg.show_cpu = 1;
    cfg.show_soc_temp = 1;
    cfg.show_cpu_clock = 1;
    cfg.show_gpu_clock = 1;
    cfg.show_fan_duty = 1;
    cfg.show_memory = 1;
    cfg.ov_mode = 1;
    sens.have_cpu = 1;
    sens.dbg_have = 1;
    for (int i = 0; i < NCORES; ++i) {
        sens.core_ok[i] = 1;
        sens.core[i] = 30.0f;
        sens.dbg_ratio[i] = 0.5f;
    }
    sens.have_fan = 1;
    sens.fan = 23;
    sens.have_ram = 1;
    sens.ram_used_mb = 2048;
    sens.ram_total_mb = 6144;
    sens.have_vram = 1;
    sens.vram_pct = 25.0f;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n <= PREFBAR_ROWS, "all advanced metrics must fit the wire row capacity (%d/%d)", n, PREFBAR_ROWS);
    CHECK(n == PREFBAR_ROWS, "all seven rows should be available in the fully populated advanced layout (got %d)", n);
}

static void test_reload_config_preserves_last_good(void)
{
    reset("show_fps=1\ndebug_notify=1\n");
    config_t active;
    CHECK(load_config(&active) == 1, "initial config should load");
    apply_debug_config(&active);
    CHECK(active.show_fps == 1 && active.debug_notify == 1,
          "initial config values should be loaded");

    FILE *f = fopen(PREFBAR_CONFIG_PATH, "w");
    CHECK(f != NULL, "open replacement config");
    if (f) {
        fputs("show_fps=0\ndebug_notify=4\noverlay_font=32\n", f);
        fclose(f);
    }
    CHECK(reload_config(&active) == 1, "valid game-entry config reload should succeed");
    CHECK(active.show_fps == 0 && active.debug_notify == 4 && active.ov_font == 32,
          "reload should apply values from one new config snapshot");
    CHECK(g_dbg.notify_level == 4, "debug notification threshold should reload too");

    const int old_show_fps = active.show_fps;
    const int old_font = active.ov_font;
    CHECK(unlink(PREFBAR_CONFIG_PATH) == 0, "remove config to simulate read failure");
    CHECK(reload_config(&active) == 0, "missing config should reject reload");
    CHECK(active.show_fps == old_show_fps && active.ov_font == old_font,
          "failed reload must preserve the last known-good settings");
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
    test_compact_overlay_rows();
    test_clock_sensor_and_row();
    test_power_sensor_and_row();
    test_reload_config_preserves_last_good();
    test_toast_cap();
    test_rotation();
    test_guards();
    test_threads();
    test_single_instance();
    printf(g_fail ? "prefbar debug selftest: %d FAILED\n" : "prefbar debug selftest: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
