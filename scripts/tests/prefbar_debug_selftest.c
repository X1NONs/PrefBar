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
    CHECK(cfg.overlay_horizontal == 0 &&
              cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_MEDIUM &&
              cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_CENTER,
          "horizontal layout should default to medium spacing and centered alignment");
    CHECK(cfg.show_fps && cfg.show_frametime && cfg.show_1percent_low &&
              cfg.show_cpu_usage && cfg.show_cpu_temp && cfg.show_cpu_clock &&
              cfg.show_gpu_temp && cfg.show_gpu_clock && cfg.show_fan_duty &&
              cfg.show_ram && cfg.show_vram && cfg.show_power && !cfg.gpu_temp_label_soc,
          "each independent telemetry toggle should default to enabled");

    FILE *defaults_file = fopen(PREFBAR_CONFIG_PATH, "r");
    char defaults_text[8192] = {0};
    if (defaults_file)
    {
        (void)fread(defaults_text, 1, sizeof(defaults_text) - 1, defaults_file);
        fclose(defaults_file);
    }
    char *overlay_section = strstr(defaults_text, "[overlay]");
    char *telemetry_section = strstr(defaults_text, "[telemetry]");
    char *horizontal_option = strstr(defaults_text, "overlay_horizontal=0");
    char *horizontal_spacing_option = strstr(defaults_text, "overlay_horizontal_spacing=medium");
    char *horizontal_align_option = strstr(defaults_text, "overlay_horizontal_align=center");
    char *gpu_temp_label_option = strstr(defaults_text, "gpu_temp_label=gpu");
    CHECK(overlay_section && telemetry_section && horizontal_option &&
              horizontal_spacing_option && horizontal_align_option && gpu_temp_label_option &&
              overlay_section < horizontal_option &&
              horizontal_option < horizontal_spacing_option &&
              horizontal_spacing_option < horizontal_align_option &&
              horizontal_align_option < telemetry_section,
          "horizontal mode, spacing, and alignment should be grouped under [overlay]");
    CHECK(strstr(defaults_text, "show_cpu_usage=1") &&
              strstr(defaults_text, "show_cpu_temp=1") &&
              strstr(defaults_text, "show_ram=1") &&
              strstr(defaults_text, "show_vram=1"),
          "default config should expose separate CPU, RAM, and VRAM controls");
    CHECK(cfg.notifications == 1 && cfg.touchpad_toggle == 1,
          "default notifications and touchpad toggle should be enabled");

    /* An older config's grouped toggles seed the new individual toggles. */
    FILE *f = fopen(PREFBAR_CONFIG_PATH, "w");
    CHECK(f != NULL, "open legacy custom config");
    if (f)
    {
        fputs("# legacy config\n[overlay]\noverlay=0\ntext_color=#123456\ntext_shadow=0\noverlay_font=32\noverlay_corner=2\noverlay_x=44\noverlay_y=55\noverlay_horizontal=1\noverlay_horizontal_spacing=long\n"
              "[notifications]\nnotifications=0\nstartup_notification=0\nerror_notifications=0\ndebug_notifications=0\ndebug_notify=0\n"
              "[telemetry]\nshow_fps=0\nshow_cpu=0\nshow_soc_temp=0\nshow_cpu_clock=0\nshow_gpu_clock=0\nshow_fan_duty=0\nshow_memory=0\nshow_power=0\n"
              "[input]\ntouchpad_toggle=0\n", f);
        fclose(f);
    }
    ensure_default_config_file();
    (void)load_config(&cfg);
    CHECK(cfg.overlay == 0 && cfg.show_fps == 0,
          "migration should preserve existing overlay and FPS settings");
    CHECK(cfg.show_frametime == 0 && cfg.show_1percent_low == 0 &&
              cfg.show_cpu_usage == 0 && cfg.show_cpu_temp == 0 &&
              cfg.show_gpu_temp == 0 && cfg.show_ram == 0 && cfg.show_vram == 0 &&
              cfg.show_cpu_clock == 0 && cfg.show_gpu_clock == 0 &&
              cfg.show_fan_duty == 0 && cfg.show_power == 0,
          "legacy group switches should seed all corresponding new controls as disabled");
    CHECK(cfg.gpu_temp_label_soc == 0,
          "missing temperature label option should migrate to the user-facing GPU label");
    CHECK(cfg.overlay_horizontal == 1 &&
              cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_LONG &&
              cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_CENTER,
          "migration should preserve existing spacing and default new alignment to center");

    f = fopen(PREFBAR_CONFIG_PATH, "w");
    CHECK(f != NULL, "open config with explicit individual settings");
    if (f)
    {
        fputs("[overlay]\noverlay=0\ntext_color=#123456\ntext_shadow=0\noverlay_font=32\noverlay_corner=2\noverlay_x=44\noverlay_y=55\noverlay_horizontal=1\noverlay_horizontal_spacing=short\noverlay_horizontal_align=right\n"
              "[notifications]\nnotifications=0\nstartup_notification=0\nerror_notifications=0\ndebug_notifications=0\ndebug_notify=0\n"
              "[telemetry]\nshow_fps=0\nshow_frametime=1\nshow_1percent_low=0\nshow_cpu_usage=0\nshow_cpu_temp=1\nshow_cpu_clock=0\nshow_gpu_temp=1\ngpu_temp_label=soc\nshow_gpu_clock=0\nshow_fan_duty=0\nshow_ram=0\nshow_vram=1\nshow_power=0\n"
              "[input]\ntouchpad_toggle=0\n", f);
        fclose(f);
    }
    ensure_default_config_file();
    (void)load_config(&cfg);
    CHECK(cfg.gpu_temp_label_soc == 1,
          "gpu_temp_label=soc should restore the old SoC wording");
    CHECK(cfg.show_fps == 0 && cfg.show_frametime == 1 &&
              cfg.show_1percent_low == 0 && cfg.show_cpu_usage == 0 &&
              cfg.show_cpu_temp == 1 && cfg.show_ram == 0 &&
              cfg.show_vram == 1 && cfg.show_power == 0,
          "explicit independent metric values should be parsed without being overwritten");
    CHECK(cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_RIGHT &&
              cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_SHORT,
          "explicit horizontal alignment and spacing should be parsed");
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
              cfg.error_notifications == 0 && cfg.debug_notifications == 0 &&
              cfg.touchpad_toggle == 0,
          "notification and input switches should parse");
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

static void test_horizontal_spacing_config(void)
{
    config_t cfg;
    reset("[overlay]\noverlay_horizontal=1\noverlay_horizontal_spacing=short\n");
    CHECK(load_config(&cfg) == 1, "short spacing config should load");
    CHECK(cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_SHORT,
          "short spacing token should select the short preset");

    reset("[overlay]\noverlay_horizontal_spacing=medium\n");
    CHECK(load_config(&cfg) == 1, "medium spacing config should load");
    CHECK(cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_MEDIUM,
          "medium spacing token should select the medium preset");

    reset("[overlay]\noverlay_horizontal_spacing=long\n");
    CHECK(load_config(&cfg) == 1, "long spacing config should load");
    CHECK(cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_LONG,
          "long spacing token should select the long preset");

    reset("[overlay]\noverlay_horizontal_spacing=invalid\n");
    CHECK(load_config(&cfg) == 1, "invalid spacing config should still load");
    CHECK(cfg.overlay_horizontal_spacing == PREFBAR_HORIZONTAL_SPACING_MEDIUM,
          "invalid spacing token should fall back to medium");
}

static void test_horizontal_align_config(void)
{
    config_t cfg;
    reset("[overlay]\noverlay_horizontal_align=left\n");
    CHECK(load_config(&cfg) == 1, "left alignment config should load");
    CHECK(cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_LEFT,
          "left token should select left alignment");

    reset("[overlay]\noverlay_horizontal_align=center\n");
    CHECK(load_config(&cfg) == 1, "center alignment config should load");
    CHECK(cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_CENTER,
          "center token should select center alignment");

    reset("[overlay]\noverlay_horizontal_align=right\n");
    CHECK(load_config(&cfg) == 1, "right alignment config should load");
    CHECK(cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_RIGHT,
          "right token should select right alignment");

    reset("[overlay]\noverlay_horizontal_align=invalid\n");
    CHECK(load_config(&cfg) == 1, "invalid alignment config should still load");
    CHECK(cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_CENTER,
          "invalid alignment should fall back to center");
}

static void test_compact_overlay_rows(void)
{
    config_t cfg;
    config_defaults(&cfg);
    cfg.overlay_format = PREFBAR_OVERLAY_FORMAT_CLASSIC;
    report_t report = {0};
    sensors_t sens = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];

    cfg.sensors = 1;
    report.fps = 60.0;
    report.avg_ms = 16.7;
    report.low1_fps = 58.0;
    sens.have_cpu = 1;
    sens.cpu_avg = 42.0f;
    sens.have_cpu_temp = 1;
    sens.cpu_temp = 65;
    sens.have_soc_temp = 1;
    sens.soc_temp = 55;
    sens.have_cpu_clock = 1;
    sens.cpu_clock_mhz = 3500;
    sens.have_gpu_clock = 1;
    sens.gpu_clock_mhz = 2150;
    sens.have_ram = 1;
    sens.ram_used_mb = 2500;
    sens.ram_total_mb = 6144;
    sens.have_vram = 1;
    sens.vram_pct = 35.0f;
    sens.have_fan = 1;
    sens.fan = 23;
    sens.have_power = 1;
    sens.power_w = 150.0f;

    cfg.show_fan_duty = 0;
    cfg.show_power = 0;
    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 10, "disabled fan and power should compact; expected 10 rows, got %d", n);
    CHECK(strncmp(rows[0], "FPS ", 4) == 0 &&
              strncmp(rows[1], "FT ", 3) == 0 &&
              strncmp(rows[2], "1%L ", 4) == 0,
          "FPS, frametime, and 1%% low should occupy independent rows");
    CHECK(strstr(rows[3], "42%") && strstr(rows[4], "65C") &&
              strstr(rows[5], "3500MHz"),
          "CPU usage, temperature, and clock should be independently rendered");
    CHECK(strstr(rows[6], "GPU 2150MHz") && strstr(rows[7], "GPU 55C"),
          "GPU clock and the default GPU temperature label should be independently rendered");
    CHECK(strncmp(rows[8], "RAM ", 4) == 0 &&
              strncmp(rows[9], "VRAM ", 5) == 0,
          "RAM and VRAM should occupy separate rows");

    cfg.show_fps = 0;
    cfg.show_frametime = 0;
    cfg.show_1percent_low = 0;
    cfg.show_cpu_usage = 0;
    cfg.show_cpu_temp = 0;
    cfg.show_cpu_clock = 0;
    cfg.show_gpu_clock = 0;
    cfg.show_gpu_temp = 0;
    cfg.show_fan_duty = 1;
    cfg.show_ram = 1;
    cfg.show_vram = 1;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 3 && strncmp(rows[0], "FAN ", 4) == 0 &&
              strncmp(rows[1], "RAM ", 4) == 0 &&
              strncmp(rows[2], "VRAM ", 5) == 0,
          "remaining enabled metrics should reflow with no gaps (n=%d)", n);

    cfg.show_ram = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 2 && strncmp(rows[0], "FAN ", 4) == 0 &&
              strncmp(rows[1], "VRAM ", 5) == 0,
          "disabling RAM should not disable VRAM or leave an empty slot");

    cfg.show_cpu_usage = 1;
    cfg.show_fan_duty = 0;
    cfg.ov_mode = 1;
    sens.dbg_have = 1;
    for (int i = 0; i < NCORES; ++i) {
        sens.core_ok[i] = 1;
        sens.core[i] = 25.0f + (float)i;
        sens.dbg_ratio[i] = 0.5f;
    }
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 4 && strncmp(rows[0], "CPU ", 4) == 0 &&
              strncmp(rows[1], "VRAM ", 5) == 0 &&
              strncmp(rows[2], "cores:", 6) == 0 &&
              strncmp(rows[3], "idle/dt:", 8) == 0,
          "advanced diagnostics should follow enabled metrics in compacted order");

    cfg.sensors = 0;
    cfg.show_fps = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && rows[0][0] == '\0',
          "all-disabled overlay must send one empty clearing row (n=%d, text='%s')", n, rows[0]);
}

static void test_overlay_format_and_compact_groups(void)
{
    config_t cfg;
    report_t report = {0};
    sensors_t sens = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];

    reset("overlay_format=compact\n");
    CHECK(load_config(&cfg) == 1, "compact format config should load");
    CHECK(cfg.overlay_format == PREFBAR_OVERLAY_FORMAT_COMPACT,
          "compact should parse as the default format token");

    report.fps = 60.0;
    report.avg_ms = 16.7;
    report.low1_fps = 58.0;
    sens.have_cpu = sens.have_cpu_temp = sens.have_cpu_clock = 1;
    sens.cpu_avg = 42.0f;
    sens.cpu_temp = 65;
    sens.cpu_clock_mhz = 3500;
    sens.have_soc_temp = sens.have_gpu_clock = 1;
    sens.soc_temp = 55;
    sens.gpu_clock_mhz = 2150;
    sens.have_ram = sens.have_vram = 1;
    sens.ram_used_mb = 2500;
    sens.ram_total_mb = 6144;
    sens.vram_pct = 35.0f;
    sens.have_fan = sens.have_power = 1;
    sens.fan = 23;
    sens.power_w = 150.0f;

    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 5, "compact format should use five grouped rows, got %d", n);
    CHECK(strstr(rows[0], "FPS 60.0") && strstr(rows[0], "FT 16.7ms") &&
              strstr(rows[0], "1%L 58.0"),
          "frame metrics should share one row: '%s'", rows[0]);
    CHECK(strstr(rows[1], "CPU: 42% | 65C | 3500MHz"),
          "CPU metrics should share a labeled row: '%s'", rows[1]);
    CHECK(strstr(rows[2], "GPU: 55C | 2150MHz"),
          "default compact output should retain the familiar combined GPU row: '%s'", rows[2]);
    cfg.gpu_temp_label_soc = 1;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 5 && strstr(rows[2], "GPU: 2150MHz | SoC: 55C"),
          "SoC label option should split the GPU clock and identify the sensor source: '%s'", rows[2]);
    cfg.gpu_temp_label_soc = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(strstr(rows[3], "MEM: RAM 2500/6144MB | VRAM 35%"),
          "memory metrics should share a row: '%s'", rows[3]);
    CHECK(strstr(rows[4], "SYS: FAN 23% | PWR 150W"),
          "fan and power should share a system row: '%s'", rows[4]);

    cfg.show_cpu_usage = 0;
    cfg.show_cpu_clock = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 5 && strcmp(rows[1], "CPU: 65C") == 0,
          "disabling CPU usage and clock must preserve CPU temperature");
    cfg.show_cpu_temp = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 4, "an empty CPU group should disappear (n=%d)", n);
    CHECK(strcmp(rows[1], "GPU: 55C | 2150MHz") == 0,
          "GPU readings should reflow with no empty row using the default label");

    reset("overlay_format=classic\n");
    CHECK(load_config(&cfg) == 1, "classic format config should load");
    CHECK(cfg.overlay_format == PREFBAR_OVERLAY_FORMAT_CLASSIC,
          "classic token should preserve the old one-metric-per-row layout");
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 12, "classic format should retain all twelve available metric rows, got %d", n);

    reset("overlay_format=invalid\n");
    CHECK(load_config(&cfg) == 1, "invalid format config should still load");
    CHECK(cfg.overlay_format == PREFBAR_OVERLAY_FORMAT_COMPACT,
          "invalid format should fall back to compact");

    reset("overlay_horizontal=1\n");
    migrate_config_options();
    char migrated[32] = "";
    CHECK(read_config_value("overlay_format", migrated, sizeof migrated) &&
              strcmp(migrated, "compact") == 0,
          "upgrade migration should add compact format without replacing the existing config");
}

static void test_clock_sensor_and_row(void)
{
    reset("show_cpu_clock=1\nshow_gpu_clock=1\n");
    config_t cfg;
    CHECK(load_config(&cfg) == 1, "clock config should load");
    cfg.overlay_format = PREFBAR_OVERLAY_FORMAT_CLASSIC;

    sensors_t sens;
    cpuu_t cpuu = {0};
    sensors_read(&sens, &cpuu, now_ns());
    CHECK(sens.have_cpu_clock && sens.cpu_clock_mhz == 3500,
          "simulated CPU frequency should convert to 3500 MHz");
    CHECK(sens.have_gpu_clock && sens.gpu_clock_mhz == 2150,
          "SoC clock domain 20 should provide GPU GFXCLK in MHz");
    CHECK(sens.have_soc_temp && sens.soc_temp >= 55,
          "SoC sensor 0 should be exposed as SoC sensor temperature");

    cfg.sensors = 1;
    cfg.show_fps = 0;
    cfg.show_frametime = 0;
    cfg.show_1percent_low = 0;
    cfg.show_cpu_usage = 0;
    cfg.show_cpu_temp = 0;
    cfg.show_cpu_clock = 1;
    cfg.show_gpu_clock = 1;
    cfg.show_gpu_temp = 1;
    cfg.show_fan_duty = 0;
    cfg.show_ram = 0;
    cfg.show_vram = 0;
    cfg.show_power = 0;
    report_t report = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];
    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 3 && strstr(rows[0], "3500MHz") &&
              strstr(rows[1], "GPU 2150MHz") && strstr(rows[2], "GPU 55C"),
          "CPU clock, GPU clock, and default GPU temperature label should be independent rows (n=%d)", n);

    char sensor_text[256];
    format_sensors(&sens, cfg.gpu_temp_label_soc, sensor_text, sizeof sensor_text);
    CHECK(strstr(sensor_text, "GPU 55C 2150MHz"),
          "human-readable status should use the GPU label by default: '%s'", sensor_text);
    cfg.gpu_temp_label_soc = 1;
    n = build_rows(&cfg, &report, &sens, rows);
    format_sensors(&sens, cfg.gpu_temp_label_soc, sensor_text, sizeof sensor_text);
    CHECK(n == 3 && strstr(rows[2], "SoC 55C") &&
              strstr(sensor_text, "GPU 2150MHz | SoC 55C"),
          "SoC label option should apply to both overlay and status text");

    char kv[1024];
    format_kv(&report, &sens, kv, sizeof kv);
    CHECK(strstr(kv, "cpu_clock_mhz=3500\n") &&
              strstr(kv, "gpu_clock_mhz=2150\n") &&
              strstr(kv, "gpu_temp_c=55\n") &&
              strstr(kv, "soc_temp_c=55\n"),
          "status.kv should expose new keys and retain the legacy temperature key");
}

static void test_power_sensor_and_row(void)
{
    reset("show_power=1\noverlay_horizontal=1\n");
    config_t cfg;
    CHECK(load_config(&cfg) == 1, "power config should load");
    CHECK(cfg.show_power == 1, "show_power should be enabled by config");
    CHECK(cfg.overlay_horizontal == 1, "overlay_horizontal should parse as enabled");
    CHECK(cfg.overlay_horizontal_align == PREFBAR_HORIZONTAL_ALIGN_CENTER,
          "horizontal alignment should default to center");

    sensors_t sens;
    cpuu_t cpuu = {0};
    sensors_read(&sens, &cpuu, now_ns());
    CHECK(sens.have_power == 1, "simulated SoC power API should provide a sample");
    CHECK(sens.power_w > 100.0f && sens.power_w < 300.0f,
          "power should be converted from milliwatts to watts (got %.2f W)", sens.power_w);

    config_defaults(&cfg);
    cfg.sensors = 1;
    cfg.show_fps = 0;
    cfg.show_frametime = 0;
    cfg.show_1percent_low = 0;
    cfg.show_cpu_usage = 0;
    cfg.show_cpu_temp = 0;
    cfg.show_cpu_clock = 0;
    cfg.show_gpu_temp = 0;
    cfg.show_gpu_clock = 0;
    cfg.show_fan_duty = 0;
    cfg.show_ram = 0;
    cfg.show_vram = 0;
    cfg.show_power = 1;
    cfg.overlay_format = PREFBAR_OVERLAY_FORMAT_CLASSIC;
    report_t report = {0};
    char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN];
    int n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && strncmp(rows[0], "PWR ", 4) == 0 && strstr(rows[0], "W") != NULL,
          "PWR should remain independently controllable (n=%d, row='%s')", n, rows[0]);

    cfg.show_power = 0;
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == 1 && rows[0][0] == '\0', "disabled PWR should not leave a row");

    config_defaults(&cfg);
    cfg.overlay_format = PREFBAR_OVERLAY_FORMAT_CLASSIC;
    cfg.sensors = 1;
    cfg.ov_mode = 1;
    report.fps = 60.0;
    report.avg_ms = 16.7;
    report.low1_fps = 58.0;
    sens.have_cpu = 1;
    sens.cpu_avg = 30.0f;
    sens.have_cpu_temp = 1;
    sens.cpu_temp = 60;
    sens.have_cpu_clock = 1;
    sens.cpu_clock_mhz = 3500;
    sens.have_gpu_clock = 1;
    sens.gpu_clock_mhz = 2150;
    sens.have_soc_temp = 1;
    sens.soc_temp = 55;
    sens.have_fan = 1;
    sens.fan = 23;
    sens.have_ram = 1;
    sens.ram_used_mb = 2048;
    sens.ram_total_mb = 6144;
    sens.have_vram = 1;
    sens.vram_pct = 25.0f;
    sens.have_power = 1;
    sens.power_w = 150.0f;
    sens.dbg_have = 1;
    for (int i = 0; i < NCORES; ++i) {
        sens.core_ok[i] = 1;
        sens.core[i] = 30.0f;
        sens.dbg_ratio[i] = 0.5f;
    }
    n = build_rows(&cfg, &report, &sens, rows);
    CHECK(n == PREFBAR_ROWS,
          "all 12 independent metrics plus two diagnostics should fit (%d/%d)", n, PREFBAR_ROWS);
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
    test_horizontal_spacing_config();
    test_horizontal_align_config();
    test_compact_overlay_rows();
    test_overlay_format_and_compact_groups();
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
