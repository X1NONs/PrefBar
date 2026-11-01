// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * PrefBar telemetry engine. Runs as a thread inside the controller payload.
 *
 * Reports, once per second:
 *   - FPS, average frametime, 1% low, worst frametime, hitch count
 *   - CPU usage (average + per core), CPU temp, SoC temp, fan duty
 *   - RAM and VRAM usage
 * to <DATA_DIR>/status.txt (human readable) and <DATA_DIR>/status.kv
 * (key=value, for a future overlay / scripts), plus an optional toast.
 *
 * Debug events (0.2.0): lifecycle milestones, sensor/display availability and
 * failures are appended to <DATA_DIR>/debug.log and, depending on
 * "debug_notify" in prefbar.conf, shown as system toasts (see Docs/HOW_IT_WORKS.md).
 *
 * What it deliberately does NOT do
 *   - no kernel memory access and no game-process memory writes
 *   - no direct ShellUI interaction; the controller and renderer are separate
 *   - display data is sent only to the local PrefBar renderer over loopback UDP
 *
 * Provenance (all GPL-3.0, so this file is GPL-3.0-or-later)
 *   FPS source : /dev/dce flip counter - ioctl, arguments and counter offset
 *                from etaHEN/OnionHEN fps_dce.cpp
 *                (c) 2026 OnionHEN / LightningMods, following PHU Games
 *                Tools by ArkSama.
 *   CPU usage  : etaHEN shellui/src/prx.cpp - per-core usage from the
 *                SceIdleCpu0..7 idle threads via sceKernelGetCpuUsage.
 *   RAM / VRAM : etaHEN shellui/src/prx.cpp - get_page_table_stats(1, type).
 *   Temps      : sceKernelGetCpuTemperature / sceKernelGetSocSensorTemperature
 *                (etaHEN labels the SoC sensor "GPU temp").
 *
 * Build
 *   PS5 payload:  make PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk   (see Makefile)
 *   Host test:    make test    (simulated display/sensors and protocol checks)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef PREFBAR_EMBED
#include <pthread.h>
#endif
#include "prefbar/telemetry_wire.h"
#ifndef PREFBAR_SIM
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
/* Published by the C++ controller after its game-process stability gate. */
extern int prefbar_game_process_ready(void);
#endif

#ifdef PREFBAR_SIM
#define DATA_DIR "/tmp/prefbar"
#define LOCK_PATH "/tmp/prefbar.lock"
#else
#define DATA_DIR "/data/prefbar"
#define LOCK_PATH "/system_tmp/prefbar.lock"
#endif

#define PREFBAR_CONFIG_PATH DATA_DIR "/prefbar.conf"

#define HIST 600             /* frametimes kept for avg / 1% low        */
#define MAX_FLIPS_PER_POLL 8 /* cap if the poll loop was stalled        */
#define REPORT_NS 1000000000ull
#define STALE_NS 2000000000ull /* no counter change for this long = idle */
#define NCORES 8
#define MAX_THREADS 3072

/* ------------------------------------------------------------------ time */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------- debug events
 * dbg_event() is the single entry point. Each event is appended to
 * <DATA_DIR>/debug.log and, when its level is <= "debug_notify" from
 * prefbar.conf, shown as a system toast.
 *   debug_notify = 0 off | 1 errors | 2 +warnings | 3 +milestones (default) | 4 +trace
 * Flood protection: one event per key per cooldown, and at most DBG_MAX_TOASTS
 * toasts per run, so a fault loop can never bury the screen in notifications.
 * Safe to call from the controller thread and the sampler thread at once.
 */
#define PREFBAR_VERSION "0.2.0"

#define DBG_ERR 1
#define DBG_WARN 2
#define DBG_INFO 3
#define DBG_TRACE 4

#define DBG_MAX_KEYS 40
#define DBG_MAX_TOASTS 30
#define DBG_LOG_MAX (64 * 1024)
#define DBG_STARTUP_NOTICE_DELAY_NS (3ull * 1000000000ull)

#if defined(PREFBAR_SIM)
/* host simulation: show toasts on stderr so the logic can be tested */
static int g_sim_toasts; /* counted for tests/prefbar_debug_selftest.c */
static void notify(const char *msg)
{
    g_sim_toasts++;
    fprintf(stderr, "[toast] %s\n", msg);
}
#elif defined(PREFBAR_NOTIFY)
/* same request layout etaHEN / OnionHEN use (0xC30 bytes) */
typedef struct
{
    int32_t type;            /* 0x00 */
    int32_t req_id;          /* 0x04 */
    int32_t priority;        /* 0x08 */
    int32_t msg_id;          /* 0x0C */
    int32_t target_id;       /* 0x10 */
    int32_t user_id;         /* 0x14 */
    int32_t unk1;            /* 0x18 */
    int32_t unk2;            /* 0x1C */
    int32_t app_id;          /* 0x20 */
    int32_t error_num;       /* 0x24 */
    int32_t unk3;            /* 0x28 */
    char use_icon_image_uri; /* 0x2C */
    char message[1024];      /* 0x2D */
    char uri[1024];          /* 0x42D */
    char unkstr[1024];       /* 0x82D */
} notify_req_t;
typedef char notify_size_check[(sizeof(notify_req_t) == 0xC30) ? 1 : -1];
/* weak, like the sensor APIs: if the loader cannot resolve it the toast is
 * skipped instead of breaking the whole payload */
extern int sceKernelSendNotificationRequest(int, notify_req_t *, size_t, int) __attribute__((weak));

static void notify(const char *msg)
{
    if (!sceKernelSendNotificationRequest)
        return;
    notify_req_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s", msg);
    req.type = 0;
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    snprintf(req.uri, sizeof req.uri, "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#else
static void notify(const char *msg) { (void)msg; }
#endif

typedef struct
{
    uint32_t h;
    uint64_t last_ns;
    unsigned suppressed;
} dbg_slot_t;

static struct
{
    int ready;
    int notify_level;
    int notifications_enabled;
    int startup_notification;
    int error_notifications;
    int debug_notifications;
    int toasts;
    int nslots;
    uint64_t t0;
    int startup_notice_sent;
    dbg_slot_t slot[DBG_MAX_KEYS];
} g_dbg;

#ifdef PREFBAR_EMBED
static pthread_mutex_t g_dbg_mu = PTHREAD_MUTEX_INITIALIZER;
#define DBG_LOCK() pthread_mutex_lock(&g_dbg_mu)
#define DBG_UNLOCK() pthread_mutex_unlock(&g_dbg_mu)
#if !defined(PREFBAR_SIM)
static pthread_mutex_t g_dbg_toast_mu = PTHREAD_MUTEX_INITIALIZER;
#endif
#else
#define DBG_LOCK() ((void)0)
#define DBG_UNLOCK() ((void)0)
#endif

static uint32_t dbg_hash(const char *s) /* FNV-1a */
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static uint64_t dbg_cooldown_ns(int level)
{
    switch (level)
    {
    case DBG_ERR:
    case DBG_WARN:
        return 30ull * 1000000000ull;
    case DBG_INFO:
        return 3ull * 1000000000ull;
    default:
        return 15ull * 1000000000ull;
    }
}

/* Small, dependency-free key=value configuration reader. Unknown keys and
 * malformed values are ignored; callers provide safe defaults. */
static void trim_config_text(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        ++start;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static int read_config_value(const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size == 0)
        return 0;
    FILE *f = fopen(PREFBAR_CONFIG_PATH, "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f))
    {
        trim_config_text(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        trim_config_text(line);
        trim_config_text(eq);
        if (strcmp(line, key) == 0)
        {
            snprintf(out, out_size, "%s", eq);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static int read_config_int(const char *key, int fallback, int min_value, int max_value)
{
    char value[64], *end = NULL;
    if (!read_config_value(key, value, sizeof value))
        return fallback;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < min_value || parsed > max_value)
        return fallback;
    return (int)parsed;
}

static int read_config_rgb(const char *key, float *r, float *g, float *b)
{
    char value[32];
    if (!read_config_value(key, value, sizeof value))
        return 0;
    if (value[0] != '#' || strlen(value) != 7)
        return 0;
    char *end = NULL;
    errno = 0;
    long rgb = strtol(value + 1, &end, 16);
    if (errno || end == value + 1 || *end || rgb < 0 || rgb > 0xffffff)
        return 0;
    *r = (float)((rgb >> 16) & 0xff) / 255.0f;
    *g = (float)((rgb >> 8) & 0xff) / 255.0f;
    *b = (float)(rgb & 0xff) / 255.0f;
    return 1;
}

static void ensure_default_config_file(void)
{
    static const char defaults[] =
        "# PrefBar 0.2.0 configuration\n"
        "# Edit values and restart PrefBar to apply them.\n"
        "# Boolean values: 0 = off, 1 = on. Invalid values use defaults.\n\n"
        "[notifications]\n"
        "notifications=1\n"
        "startup_notification=1\n"
        "error_notifications=1\n"
        "debug_notifications=1\n"
        "debug_notify=3\n"
        "notify_sec=0\n\n"
        "[overlay]\n"
        "overlay=1\n"
        "text_color=#F2F2F2\n"
        "text_shadow=1\n"
        "overlay_mode=0\n"
        "overlay_font=26\n"
        "overlay_corner=0\n"
        "overlay_x=24\n"
        "overlay_y=24\n\n"
        "[telemetry]\n"
        "sensors=1\n"
        "show_fps=1\n"
        "show_cpu=1\n"
        "show_soc_temp=1\n"
        "show_fan_duty=1\n"
        "show_memory=1\n\n"
        "[input]\n"
        "touchpad_toggle=1\n\n"
        "[diagnostics]\n"
        "diag=0\n"
        "poll_us=2000\n";
    (void)mkdir(DATA_DIR, 0777);
    int fd = open(PREFBAR_CONFIG_PATH, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return; /* never overwrite an existing user configuration */
    const size_t len = sizeof(defaults) - 1;
    const char *cur = defaults;
    size_t left = len;
    int failed = 0;
    while (left)
    {
        ssize_t n = write(fd, cur, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            failed = 1;
            break;
        }
        cur += n;
        left -= (size_t)n;
    }
    close(fd);
    if (failed)
        (void)unlink(PREFBAR_CONFIG_PATH);
}

/* caller holds the lock */
static void dbg_init_locked(void)
{
    if (g_dbg.ready)
        return;
    g_dbg.ready = 1;
    g_dbg.notify_level = DBG_INFO;
    g_dbg.t0 = now_ns();
    ensure_default_config_file();
    g_dbg.notify_level = read_config_int("debug_notify", 3, 0, DBG_TRACE);
    g_dbg.notifications_enabled = read_config_int("notifications", 1, 0, 1);
    g_dbg.startup_notification = read_config_int("startup_notification", 1, 0, 1);
    g_dbg.error_notifications = read_config_int("error_notifications", 1, 0, 1);
    g_dbg.debug_notifications = read_config_int("debug_notifications", 1, 0, 1);
}

/* caller holds the lock; keeps debug.log under DBG_LOG_MAX (+ one .1 backup) */
static void dbg_log_locked(uint64_t t_ns, int level, const char *key, const char *msg)
{
    static const char *const names[] = {"?", "ERR ", "WARN", "INFO", "TRC "};
    char path[160], old[176];
    struct stat st;
    snprintf(path, sizeof path, "%s/debug.log", DATA_DIR);
    if (stat(path, &st) == 0 && st.st_size > DBG_LOG_MAX)
    {
        snprintf(old, sizeof old, "%s.1", path);
        rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "[%9.3f] %s %-16s %s\n", (double)(t_ns - g_dbg.t0) / 1e9,
            names[(level >= 1 && level <= 4) ? level : 0], key, msg);
    fclose(f);
}

/* Ensure attribution appears once, after the first three seconds, before any debug toast.
 * The event log is never delayed; only system notifications are gated. */
static void dbg_startup_tick(void)
{
#if !defined(PREFBAR_SIM)
    int send_attribution = 0;
#if defined(PREFBAR_EMBED)
    pthread_mutex_lock(&g_dbg_toast_mu);
#endif
    DBG_LOCK();
    dbg_init_locked();
    const uint64_t t = now_ns();
    if (!g_dbg.startup_notice_sent && t >= g_dbg.t0 &&
        t - g_dbg.t0 >= DBG_STARTUP_NOTICE_DELAY_NS)
    {
        g_dbg.startup_notice_sent = 1;
        if (g_dbg.notifications_enabled && g_dbg.startup_notification &&
            g_dbg.notify_level >= DBG_INFO && g_dbg.toasts < DBG_MAX_TOASTS)
        {
            ++g_dbg.toasts;
            send_attribution = 1;
        }
    }
    DBG_UNLOCK();
    if (send_attribution)
        notify("PrefBar made by X1NONs");
#if defined(PREFBAR_EMBED)
    pthread_mutex_unlock(&g_dbg_toast_mu);
#endif
#endif
}

static void dbg_vevent(int level, const char *key, const char *fmt, va_list ap)
{
    dbg_startup_tick();
    uint32_t h = dbg_hash(key);
    unsigned supp = 0;
    int toast = 0;
    char msg[200];

    DBG_LOCK();
    dbg_init_locked();
    const uint64_t t = now_ns(); /* after init: never earlier than t0 */
    dbg_slot_t *sl = NULL;
    for (int i = 0; i < g_dbg.nslots; i++)
        if (g_dbg.slot[i].h == h)
        {
            sl = &g_dbg.slot[i];
            break;
        }
    if (!sl && g_dbg.nslots < DBG_MAX_KEYS)
    {
        sl = &g_dbg.slot[g_dbg.nslots++];
        sl->h = h;
        sl->last_ns = 0;
        sl->suppressed = 0;
    }
    if (!sl && level != DBG_ERR)
    {
        DBG_UNLOCK();
        return;
    } /* key table full */
    if (sl)
    {
        if (sl->last_ns && (t - sl->last_ns) < dbg_cooldown_ns(level))
        {
            sl->suppressed++;
            DBG_UNLOCK();
            return;
        }
        supp = sl->suppressed;
        sl->suppressed = 0;
        sl->last_ns = t;
    }
    vsnprintf(msg, sizeof msg, fmt, ap);
    if (supp)
    {
        size_t l = strlen(msg);
        snprintf(msg + l, sizeof msg - l, " (+%u more)", supp);
    }
    dbg_log_locked(t, level, key, msg);
    const int category_enabled = (level == DBG_ERR)
                                     ? g_dbg.error_notifications
                                     : g_dbg.debug_notifications;
#if defined(PREFBAR_SIM)
    if (g_dbg.notifications_enabled && category_enabled &&
        level <= g_dbg.notify_level && g_dbg.toasts < DBG_MAX_TOASTS)
    {
        g_dbg.toasts++;
        toast = 1;
    }
#else
    if (g_dbg.startup_notice_sent && g_dbg.notifications_enabled && category_enabled &&
        level <= g_dbg.notify_level && g_dbg.toasts < DBG_MAX_TOASTS)
    {
        g_dbg.toasts++;
        toast = 1;
    }
#endif
    DBG_UNLOCK();

    if (toast)
    { /* outside the lock: may block briefly */
        char out[256];
        snprintf(out, sizeof out, "%s%s",
                 level == DBG_ERR ? "PrefBar ERROR: " : level == DBG_WARN ? "PrefBar warning: "
                                                                          : "PrefBar: ",
                 msg);
#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
        pthread_mutex_lock(&g_dbg_toast_mu);
        notify(out);
        pthread_mutex_unlock(&g_dbg_toast_mu);
#else
        notify(out);
#endif
    }
}

static void dbg_event(int level, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void dbg_event(int level, const char *key, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dbg_vevent(level, key, fmt, ap);
    va_end(ap);
}

#ifdef PREFBAR_EMBED
/* Shared event entry point used by the PrefBar controller. */
void prefbar_debug_event(int level, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void prefbar_debug_event(int level, const char *key, const char *fmt, ...)
{
    if (!key || !fmt)
        return;
    if (level < DBG_ERR)
        level = DBG_ERR;
    if (level > DBG_TRACE)
        level = DBG_TRACE;
    va_list ap;
    va_start(ap, fmt);
    dbg_vevent(level, key, fmt, ap);
    va_end(ap);
}
#endif

/* ------------------------------------------------------- /dev/dce source */
#ifndef PREFBAR_SIM

#define DCE_IOCTL_ZERO 0x0000000080308217UL /* conventional zero extension */
#define DCE_IOCTL_SIGN 0xFFFFFFFF80308217UL /* PHU passes it sign-extended */
#define DCE_ARG0 0x10000000AULL
#define DCE_ARG1 0x8000000000ULL
#define DCE_OUT_BYTES 0x60
#define DCE_COUNT_OFF 8 /* flip counter at out + 8     */

struct dce_arg
{ /* must be 0x30 bytes          */
    uint64_t selector;
    uint64_t mask;
    uint64_t output;
    uint64_t reserved[3];
};
typedef char dce_arg_size_check[(sizeof(struct dce_arg) == 0x30) ? 1 : -1];

typedef struct
{
    int fd;
    int abi; /* 0 = unknown, 1 = zero-extended, 2 = sign-extended */
    int last_errno;
} dce_t;

static int dce_open(dce_t *d)
{
    if (d->fd >= 0)
        return 0;
    d->fd = open("/dev/dce", O_RDWR);
    if (d->fd < 0)
    {
        d->last_errno = errno;
        return -1;
    }
    return 0;
}

static int dce_ioctl_once(dce_t *d, unsigned long req, unsigned char *out)
{
    struct dce_arg a;
    memset(out, 0, DCE_OUT_BYTES);
    memset(&a, 0, sizeof a);
    a.selector = DCE_ARG0;
    a.mask = DCE_ARG1;
    a.output = (uint64_t)(uintptr_t)out;
    return ioctl(d->fd, req, &a);
}

/* returns 0 and the flip count on success, -1 on failure */
static int dce_read(dce_t *d, uint64_t *count)
{
    unsigned char out[DCE_OUT_BYTES];
    if (dce_open(d) != 0)
        return -1;

    unsigned long req = (d->abi == 2) ? DCE_IOCTL_SIGN : DCE_IOCTL_ZERO;
    int rc = dce_ioctl_once(d, req, out);
    int err = rc < 0 ? errno : 0;

    /* some syscall paths reject the zero-extended value: retry sign-extended */
    if (rc < 0 && err == EINVAL && req != DCE_IOCTL_SIGN)
    {
        req = DCE_IOCTL_SIGN;
        rc = dce_ioctl_once(d, req, out);
        err = rc < 0 ? errno : 0;
    }
    if (rc < 0)
    {
        d->last_errno = err;
        if (err == EBADF)
        {
            close(d->fd);
            d->fd = -1;
        }
        return -1;
    }
    d->abi = (req == DCE_IOCTL_SIGN) ? 2 : 1;
    memcpy(count, out + DCE_COUNT_OFF, sizeof *count);
    return 0;
}

#else /* ---------------------------------------------------- host simulator */

/* Simulated 60 Hz display: every 50th frame arrives 20 ms late (a hitch). */
typedef struct
{
    int fd;
    int abi;
    int last_errno;
    uint64_t t0;
} dce_t;
#define SIM_FRAMES 4000

static int dce_read(dce_t *d, uint64_t *count)
{
    static double stamp[SIM_FRAMES];
    static int init;
    if (!init)
    {
        double t = 0.0;
        for (int i = 0; i < SIM_FRAMES; i++)
        {
            t += 1000.0 / 60.0 + ((i % 50 == 49) ? 20.0 : 0.0);
            stamp[i] = t; /* ms since start */
        }
        init = 1;
    }
    if (!d->t0)
        d->t0 = now_ns();
    double ms = (double)(now_ns() - d->t0) / 1e6;
    uint64_t n = 0;
    while (n < SIM_FRAMES && stamp[n] <= ms)
        n++;
    *count = 1000 + n; /* arbitrary non-zero base */
    return 0;
}
#endif

/* ------------------------------------------------- sensor API bindings
 * Real build: the sceKernel* / get_page_table_stats symbols are declared WEAK,
 * so a symbol the linker/loader cannot resolve becomes NULL and that sensor is
 * simply reported as unavailable instead of breaking the whole payload.
 * Declarations follow etaHEN's own (shellui/src/prx.cpp, daemon/msg.cpp).
 */
typedef struct
{
    int64_t sec, nsec;
} ps_ts;
struct ps_proc_stats
{                    /* 0x28 bytes, per thread  */
    int32_t lo_data; /* 0x00 */
    uint32_t td_tid; /* 0x04 */
    ps_ts user;      /* 0x08 user CPU time      */
    ps_ts sys;       /* 0x18 system CPU time    */
};
typedef char ps_stats_size_check[(sizeof(struct ps_proc_stats) == 0x28) ? 1 : -1];

#ifndef PREFBAR_SIM
extern int sceKernelGetCpuTemperature(int *) __attribute__((weak));
extern int sceKernelGetSocSensorTemperature(int, int *) __attribute__((weak));
extern int sceKernelGetCurrentFanDuty(uint16_t *, uint64_t *) __attribute__((weak));
extern int sceKernelGetCpuUsage(struct ps_proc_stats *, int32_t *) __attribute__((weak));
extern int sceKernelGetThreadName(uint32_t, char *) __attribute__((weak));
extern int get_page_table_stats(int, int, int *, int *) __attribute__((weak));
#define SYM_PRESENT(f) ((void *)(f) != NULL)
#else
#define SYM_PRESENT(f) 1
/* simulated sensors so the logic can be tested on a host */
static double sim_elapsed(void)
{
    static uint64_t t0;
    if (!t0)
        t0 = now_ns();
    return (double)(now_ns() - t0) / 1e9;
}
static int sceKernelGetCpuTemperature(int *t)
{
    *t = 60 + (int)sim_elapsed();
    return 0;
}
static int sceKernelGetSocSensorTemperature(int id, int *t)
{
    (void)id;
    *t = 55 + (int)sim_elapsed();
    return 0;
}
/* fan reports failure on purpose: exercises the "unavailable" path */
static int sceKernelGetCurrentFanDuty(uint16_t *duty_q10, uint64_t *chassis_info)
{
    (void)duty_q10;
    (void)chassis_info;
    return -1;
}
static const double SIM_IDLE_FRAC[NCORES] = {0.90, 0.70, 0.50, 0.80, 0.60, 0.95, 0.70, 0.85};
static int sceKernelGetThreadName(uint32_t id, char *out)
{
    if (id >= 100 && id < 100 + NCORES)
        snprintf(out, 0x40, "SceIdleCpu%u", id - 100);
    else
        snprintf(out, 0x40, "SceWorker%u", id);
    return 0;
}
static int sceKernelGetCpuUsage(struct ps_proc_stats *out, int32_t *n)
{
    double e = sim_elapsed();
    int count = 24;
    for (int i = 0; i < count; i++)
    {
        double v = (i < NCORES) ? e * SIM_IDLE_FRAC[i] : e * 0.1;
        out[i].lo_data = 0;
        out[i].td_tid = (uint32_t)(100 + i);
        out[i].user.sec = 0;
        out[i].user.nsec = 0;
        out[i].sys.sec = (int64_t)v;
        out[i].sys.nsec = (int64_t)((v - (double)(int64_t)v) * 1e9);
    }
    *n = count;
    return 0;
}
static int get_page_table_stats(int vm, int type, int *total, int *fr)
{
    (void)vm;
    if (type == 1)
    {
        *total = 12288;
        *fr = 4096 + ((int)(sim_elapsed() * 100) % 512);
    }
    else
    {
        *total = 4096;
        *fr = 1024;
    }
    return 0;
}
#endif

/* ----------------------------------------------------- frame statistics */
typedef struct
{
    float ft_ms[HIST];
    int head, count;
    uint64_t last_flip_ns; /* time of the last observed counter change */
    uint64_t last_count;
    int have_count;
    uint64_t win_start_ns, win_start_count; /* 1 s window for headline FPS */
} stats_t;

static void stats_push(stats_t *s, float ms)
{
    s->ft_ms[s->head] = ms;
    s->head = (s->head + 1) % HIST;
    if (s->count < HIST)
        s->count++;
}

static void stats_update(stats_t *s, uint64_t count, uint64_t t_ns)
{
    if (!s->have_count)
    {
        s->have_count = 1;
        s->last_count = count;
        s->last_flip_ns = t_ns;
        s->win_start_ns = t_ns;
        s->win_start_count = count;
        return;
    }
    if (count < s->last_count)
    { /* counter reset / wrap */
        s->last_count = count;
        s->last_flip_ns = t_ns;
        s->win_start_ns = t_ns;
        s->win_start_count = count;
        return;
    }
    uint64_t flips = count - s->last_count;
    if (flips)
    {
        /* frametime is quantised by the poll interval; if several flips
         * happened between two polls, spread the interval evenly */
        if (flips > MAX_FLIPS_PER_POLL)
            flips = MAX_FLIPS_PER_POLL;
        float ms = (float)(t_ns - s->last_flip_ns) / 1e6f / (float)flips;
        for (uint64_t i = 0; i < flips; i++)
            stats_push(s, ms);
        s->last_count = count;
        s->last_flip_ns = t_ns;
    }
}

static int cmp_desc(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) - (x > y);
}

typedef struct
{
    double fps, avg_ms, low1_fps, max_ms;
    int samples, hitches, idle;
} report_t;

static report_t stats_report(stats_t *s, uint64_t count, uint64_t t_ns)
{
    report_t r;
    memset(&r, 0, sizeof r);
    uint64_t dt = t_ns - s->win_start_ns;
    if (dt > 0 && count >= s->win_start_count)
        r.fps = (double)(count - s->win_start_count) * 1e9 / (double)dt;
    s->win_start_ns = t_ns;
    s->win_start_count = count;

    r.idle = (t_ns - s->last_flip_ns) > STALE_NS;
    r.samples = s->count;
    if (s->count >= 2)
    {
        float tmp[HIST];
        double sum = 0.0;
        memcpy(tmp, s->ft_ms, sizeof(float) * (size_t)s->count);
        for (int i = 0; i < s->count; i++)
            sum += tmp[i];
        r.avg_ms = sum / s->count;
        /* hitch = frame took > 2x the average and > 20 ms (in the window) */
        for (int i = 0; i < s->count; i++)
            if (tmp[i] > 2.0 * r.avg_ms && tmp[i] > 20.0f)
                r.hitches++;
        qsort(tmp, (size_t)s->count, sizeof(float), cmp_desc);
        int k = s->count / 100;
        if (k < 1)
            k = 1;
        double worst = 0.0;
        for (int i = 0; i < k; i++)
            worst += tmp[i];
        worst /= k;
        r.low1_fps = worst > 0.0 ? 1000.0 / worst : 0.0;
        r.max_ms = tmp[0];
    }
    return r;
}

/* --------------------------------------------------------------- sensors */
typedef struct
{
    int have_cpu_temp, cpu_temp;
    int have_soc_temp, soc_temp;
    int have_fan, fan;
    int have_cpu, ncores_known;
    float core[NCORES], cpu_avg;
    int core_ok[NCORES];
    int have_ram, ram_used_mb, ram_total_mb;
    float ram_pct;
    int have_vram;
    float vram_pct;
    int vram_used_mb, vram_total_mb;
    int fan_called, fan_rc;
    uint64_t fan_chassis;
    uint16_t fan_duty_q10; /* API duty is Q10 (0..1024), not RPM */
    int dbg_have;
    double dbg_dt;
    float dbg_ratio[NCORES]; /* unclamped idle/dt (overlay debug) */
} sensors_t;

typedef struct
{ /* CPU-usage tracking state */
    uint32_t idle_tid[NCORES];
    int idle_mask; /* bit i = idle thread i known */
    int scanned;   /* thread names already scanned */
    double prev_idle_s[NCORES];
    uint64_t prev_t_ns;
    int have_prev;
    ps_ts raw_user[NCORES], raw_sys[NCORES]; /* last raw idle-thread times (diag) */
    int raw_found, raw_nthreads;
} cpuu_t;

static struct ps_proc_stats g_threads[MAX_THREADS];

static double ts_sec(ps_ts t) { return (double)t.sec + (double)t.nsec / 1e9; }

static void cpu_usage_sample(cpuu_t *c, sensors_t *s, uint64_t t_ns)
{
    if (!SYM_PRESENT(sceKernelGetCpuUsage))
        return;
    int32_t n = MAX_THREADS;
    if (sceKernelGetCpuUsage(g_threads, &n) != 0 || n <= 0)
        return;
    if (n > MAX_THREADS)
        n = MAX_THREADS;

    /* find the per-core idle threads ("SceIdleCpu0".."SceIdleCpu7") once */
    if (!c->scanned && SYM_PRESENT(sceKernelGetThreadName))
    {
        for (int i = 0; i < n; i++)
        {
            char name[0x40];
            int core = -1;
            memset(name, 0, sizeof name);
            if (sceKernelGetThreadName(g_threads[i].td_tid, name) == 0 &&
                sscanf(name, "SceIdleCpu%d", &core) == 1 &&
                core >= 0 && core < NCORES)
            {
                c->idle_tid[core] = g_threads[i].td_tid;
                c->idle_mask |= 1 << core;
            }
        }
        c->scanned = 1;
    }
    if (!c->idle_mask)
        return;

    double idle_s[NCORES] = {0};
    int found = 0;
    for (int core = 0; core < NCORES; core++)
    {
        if (!(c->idle_mask & (1 << core)))
            continue;
        for (int i = 0; i < n; i++)
        {
            if (g_threads[i].td_tid == c->idle_tid[core])
            {
                idle_s[core] = ts_sec(g_threads[i].user) + ts_sec(g_threads[i].sys);
                c->raw_user[core] = g_threads[i].user;
                c->raw_sys[core] = g_threads[i].sys;
                found |= 1 << core;
                break;
            }
        }
    }

    c->raw_found = found;
    c->raw_nthreads = n;
    if (c->have_prev && t_ns > c->prev_t_ns)
    {
        double dt = (double)(t_ns - c->prev_t_ns) / 1e9;
        double sum = 0.0;
        int cnt = 0;
        for (int core = 0; core < NCORES; core++)
        {
            s->core_ok[core] = 0;
            if (!(found & (1 << core)))
                continue;
            double idle = (idle_s[core] - c->prev_idle_s[core]) / dt;
            s->dbg_ratio[core] = (float)idle; /* before clamping */
            s->dbg_dt = dt;
            s->dbg_have = 1;
            if (idle > 1.0)
                idle = 1.0;
            if (idle < 0.0)
                idle = 0.0;
            s->core[core] = (float)((1.0 - idle) * 100.0);
            s->core_ok[core] = 1;
            sum += s->core[core];
            cnt++;
        }
        if (cnt)
        {
            s->cpu_avg = (float)(sum / cnt);
            s->ncores_known = cnt;
            s->have_cpu = 1;
        }
    }
    for (int core = 0; core < NCORES; core++)
        c->prev_idle_s[core] = idle_s[core];
    c->prev_t_ns = t_ns;
    c->have_prev = 1;
}

static void sensors_read(sensors_t *s, cpuu_t *c, uint64_t t_ns)
{
    memset(s, 0, sizeof *s);

    int v = 0;
    if (SYM_PRESENT(sceKernelGetCpuTemperature) && sceKernelGetCpuTemperature(&v) == 0 && v > 0)
    {
        s->have_cpu_temp = 1;
        s->cpu_temp = v;
    }
    v = 0;
    if (SYM_PRESENT(sceKernelGetSocSensorTemperature) && sceKernelGetSocSensorTemperature(0, &v) == 0 && v > 0)
    {
        s->have_soc_temp = 1;
        s->soc_temp = v;
    }

    uint16_t duty_q10 = 0;
    uint64_t chassis_info = 0;
    if (SYM_PRESENT(sceKernelGetCurrentFanDuty))
    {
        int rc = sceKernelGetCurrentFanDuty(&duty_q10, &chassis_info);
        s->fan_called = 1;
        s->fan_rc = rc;
        s->fan_chassis = chassis_info;
        s->fan_duty_q10 = duty_q10;
        /* The API reports Q10 duty (0..1024), not tachometer RPM. */
        if (rc == 0 && duty_q10 <= 1024u)
        {
            s->have_fan = 1;
            s->fan = (int)(((uint32_t)duty_q10 * 100u + 512u) / 1024u);
        }
    }

    cpu_usage_sample(c, s, t_ns);

    int total = 0, fr = 0;
    if (SYM_PRESENT(get_page_table_stats) && get_page_table_stats(1, 1, &total, &fr) != -1 && total > 0)
    {
        s->have_ram = 1;
        s->ram_total_mb = total;
        s->ram_used_mb = total - fr;
        s->ram_pct = 100.0f * (float)(total - fr) / (float)total;
    }
    total = 0;
    fr = 0;
    if (SYM_PRESENT(get_page_table_stats) && get_page_table_stats(1, 2, &total, &fr) != -1 && total > 0)
    {
        s->have_vram = 1;
        s->vram_total_mb = total;
        s->vram_used_mb = total - fr;
        s->vram_pct = 100.0f * (float)(total - fr) / (float)total;
    }
}

/* ---------------------------------------------------------------- output */
static void app(char *buf, size_t n, const char *fmt, ...)
{
    size_t len = strlen(buf);
    if (len >= n - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, n - len, fmt, ap);
    va_end(ap);
}

static void format_perf(const report_t *r, char *buf, size_t n)
{
    buf[0] = 0;
    if (r->idle)
        app(buf, n, "FPS --  (display idle)");
    else
        app(buf, n, "FPS %.1f | FT %.1f ms | 1%% low %.1f | max %.1f ms | hitches %d",
            r->fps, r->avg_ms, r->low1_fps, r->max_ms, r->hitches);
}

static void format_sensors(const sensors_t *s, char *buf, size_t n)
{
    buf[0] = 0;
    const char *sep = "";
    if (s->have_cpu)
    {
        app(buf, n, "%sCPU %.0f%%", sep, s->cpu_avg);
        if (s->have_cpu_temp)
            app(buf, n, " %dC", s->cpu_temp);
        sep = " | ";
    }
    else if (s->have_cpu_temp)
    {
        app(buf, n, "%sCPU %dC", sep, s->cpu_temp);
        sep = " | ";
    }
    if (s->have_soc_temp)
    {
        app(buf, n, "%sSoC %dC", sep, s->soc_temp);
        sep = " | ";
    }
    if (s->have_fan)
    {
        app(buf, n, "%sFan %d", sep, s->fan);
        sep = " | ";
    }
    if (s->have_ram)
    {
        app(buf, n, "%sRAM %d/%d MB (%.0f%%)", sep, s->ram_used_mb, s->ram_total_mb, s->ram_pct);
        sep = " | ";
    }
    if (s->have_vram)
    {
        app(buf, n, "%sVRAM %.0f%% (%d/%d MB)", sep, s->vram_pct, s->vram_used_mb, s->vram_total_mb);
        sep = " | ";
    }
    if (!*sep)
        app(buf, n, "sensors: none available");
}

static void format_cores(const sensors_t *s, char *buf, size_t n)
{
    buf[0] = 0;
    if (!s->have_cpu)
        return;
    app(buf, n, "Cores %%:");
    for (int i = 0; i < NCORES; i++)
    {
        if (s->core_ok[i])
            app(buf, n, " %.0f", s->core[i]);
        else
            app(buf, n, " --");
    }
}

static void format_kv(const report_t *r, const sensors_t *s, char *buf, size_t n)
{
    buf[0] = 0;
    app(buf, n, "ts=%lld\n", (long long)time(NULL));
    app(buf, n, "idle=%d\n", r->idle);
    if (!r->idle)
    {
        app(buf, n, "fps=%.1f\nframetime_ms=%.2f\nlow1_fps=%.1f\nmax_ms=%.2f\nhitches=%d\n",
            r->fps, r->avg_ms, r->low1_fps, r->max_ms, r->hitches);
    }
    if (s->have_cpu)
    {
        app(buf, n, "cpu_avg=%.1f\n", s->cpu_avg);
        for (int i = 0; i < NCORES; i++)
            if (s->core_ok[i])
                app(buf, n, "cpu%d=%.1f\n", i, s->core[i]);
    }
    if (s->have_cpu_temp)
        app(buf, n, "cpu_temp_c=%d\n", s->cpu_temp);
    if (s->have_soc_temp)
        app(buf, n, "soc_temp_c=%d\n", s->soc_temp);
    if (s->have_fan)
        app(buf, n, "fan_duty=%d\n", s->fan);
    if (s->have_ram)
        app(buf, n, "ram_used_mb=%d\nram_total_mb=%d\nram_pct=%.1f\n",
            s->ram_used_mb, s->ram_total_mb, s->ram_pct);
    if (s->have_vram)
        app(buf, n, "vram_used_mb=%d\nvram_total_mb=%d\nvram_pct=%.1f\n",
            s->vram_used_mb, s->vram_total_mb, s->vram_pct);
}

static void write_file_atomic(const char *name, const char *text)
{
    char tmp[160], dst[160];
    snprintf(dst, sizeof dst, "%s/%s", DATA_DIR, name);
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", DATA_DIR, name);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
    rename(tmp, dst);
}

/* runtime configuration (loaded from <DATA_DIR>/prefbar.conf) */
typedef struct
{
    int notify_sec, poll_us, sensors, diag;
    int overlay, ov_mode, ov_font, ov_corner, ov_x, ov_y;
    int notifications, startup_notification, error_notifications, debug_notifications;
    int text_shadow, show_fps, show_cpu, show_soc_temp, show_fan_duty, show_memory;
    int touchpad_toggle;
    float text_r, text_g, text_b;
} config_t;

/* --------------------------------------------------------------- overlay
 * Formats up to PREFBAR_ROWS text rows and sends them to the ShellUI renderer
 * (prefbar-overlay) over loopback UDP. If the renderer isn't loaded the
 * datagram is simply dropped. The renderer blanks the rows if we go quiet.
 */
static int g_udp = -1;
#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
static int g_control_fd = -1;

static void overlay_control_open(void)
{
    if (g_control_fd >= 0)
        return;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        dbg_event(DBG_WARN, "overlay.control.socket",
                  "cannot create overlay control socket (errno %d); toggle notifications unavailable", errno);
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PREFBAR_CONTROL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0)
    {
        dbg_event(DBG_WARN, "overlay.control.bind",
                  "cannot bind overlay control port %u (errno %d)", (unsigned)PREFBAR_CONTROL_PORT, errno);
        close(fd);
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    g_control_fd = fd;
    dbg_event(DBG_TRACE, "overlay.control.ready",
              "overlay control receiver ready on loopback port %u", (unsigned)PREFBAR_CONTROL_PORT);
}

static void overlay_control_poll(void)
{
    if (g_control_fd < 0)
        return;
    for (;;)
    {
        char msg[48];
        ssize_t n = recv(g_control_fd, msg, sizeof msg - 1, MSG_DONTWAIT);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            dbg_event(DBG_WARN, "overlay.control.recv",
                      "overlay control receive failed (errno %d)", errno);
            return;
        }
        if (n == 0)
            return;
        msg[n] = '\0';
        if (strcmp(msg, "PBCTL1:OVERLAY:ON") == 0)
            dbg_event(DBG_INFO, "overlay.toggle.on", "overlay enabled by triple touchpad click");
        else if (strcmp(msg, "PBCTL1:OVERLAY:OFF") == 0)
            dbg_event(DBG_INFO, "overlay.toggle.off", "overlay hidden by triple touchpad click; sampler remains active");
        else
            dbg_event(DBG_TRACE, "overlay.control.invalid", "ignored unknown overlay control message");
    }
}
#endif /* PREFBAR_EMBED && !PREFBAR_SIM */

static int build_rows(const config_t *cfg, const report_t *r, const sensors_t *s,
                      char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN])
{
    int n = cfg->sensors
                ? ((cfg->ov_mode == 1) ? PREFBAR_ROWS : 4)
                : 1;
    for (int i = 0; i < PREFBAR_ROWS; ++i)
        rows[i][0] = 0;

    if (cfg->show_fps)
    {
        if (r->idle)
            app(rows[0], PREFBAR_ROW_LEN, "FPS --");
        else
            app(rows[0], PREFBAR_ROW_LEN, "FPS %.1f  %.1f ms  1%%L %.1f",
                r->fps, r->avg_ms, r->low1_fps);
    }

    if (cfg->sensors && (cfg->show_cpu || cfg->show_soc_temp))
    {
        if (cfg->show_cpu && s->have_cpu)
        {
            app(rows[1], PREFBAR_ROW_LEN, "CPU %.0f%%", s->cpu_avg);
            if (s->have_cpu_temp)
                app(rows[1], PREFBAR_ROW_LEN, " %dC", s->cpu_temp);
        }
        else if (cfg->show_cpu && s->have_cpu_temp)
        {
            app(rows[1], PREFBAR_ROW_LEN, "CPU %dC", s->cpu_temp);
        }
        if (cfg->show_soc_temp && s->have_soc_temp)
            app(rows[1], PREFBAR_ROW_LEN, "%sSoC %dC", rows[1][0] ? "  " : "", s->soc_temp);
    }

    if (cfg->sensors && cfg->show_fan_duty)
    {
        if (s->have_fan)
            app(rows[2], PREFBAR_ROW_LEN, "Fan: %d%%", s->fan);
        else
            app(rows[2], PREFBAR_ROW_LEN, "Fan: --");
    }

    if (cfg->sensors && cfg->show_memory)
    {
        if (s->have_ram)
            app(rows[3], PREFBAR_ROW_LEN, "RAM %d/%d MB", s->ram_used_mb, s->ram_total_mb);
        if (s->have_vram)
            app(rows[3], PREFBAR_ROW_LEN, "%sVRAM %.0f%%",
                rows[3][0] ? "  " : "", s->vram_pct);
    }

    if (cfg->ov_mode == 1 && cfg->sensors)
    {
        if (s->have_cpu)
        {
            app(rows[4], PREFBAR_ROW_LEN, "cores:");
            for (int i = 0; i < NCORES; ++i)
            {
                if (s->core_ok[i])
                    app(rows[4], PREFBAR_ROW_LEN, " %.0f", s->core[i]);
                else
                    app(rows[4], PREFBAR_ROW_LEN, " --");
            }
        }
        if (s->dbg_have)
        {
            app(rows[5], PREFBAR_ROW_LEN, "idle/dt:");
            for (int i = 0; i < NCORES; ++i)
                app(rows[5], PREFBAR_ROW_LEN, " %.2f", (double)s->dbg_ratio[i]);
        }
        n = PREFBAR_ROWS;
    }
    return n;
}

static void overlay_send(const config_t *cfg, char rows[PREFBAR_ROWS][PREFBAR_ROW_LEN], int n)
{
    static uint64_t seq;
    if (n < 1)
        return;
    if (g_udp < 0)
        g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0)
    {
        dbg_event(DBG_ERR, "overlay.sock", "cannot create overlay socket (errno %d): no on-screen rows", errno);
        return;
    }

    prefbar_wire_t p;
    memset(&p, 0, sizeof p);
    p.magic = PREFBAR_WIRE_MAGIC;
    p.version = PREFBAR_WIRE_VERSION;
    p.size = (uint16_t)sizeof p;
    p.sequence = ++seq;
    p.corner = (uint8_t)cfg->ov_corner;
    p.rows = (uint8_t)n;
    p.flags = (uint8_t)((cfg->text_shadow ? PREFBAR_WIRE_FLAG_TEXT_SHADOW : 0u) |
                        (cfg->touchpad_toggle ? PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE : 0u) |
                        (cfg->overlay ? PREFBAR_WIRE_FLAG_OVERLAY_ENABLED : 0u));
    p.font_size = (uint16_t)cfg->ov_font;
    p.margin_x = (float)cfg->ov_x;
    p.margin_y = (float)cfg->ov_y;
    p.r = cfg->text_r;
    p.g = cfg->text_g;
    p.b = cfg->text_b;
    p.a = 1.0f;
    for (int i = 0; i < n; i++)
    {
        size_t len = strnlen(rows[i], PREFBAR_ROW_LEN - 1);
        memcpy(p.text[i], rows[i], len); /* text[] is zeroed: NUL-terminated */
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PREFBAR_WIRE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ssize_t sent = sendto(g_udp, &p, sizeof p, 0, (struct sockaddr *)&a, sizeof a);
    if (sent != (ssize_t)sizeof p)
        dbg_event(DBG_WARN, "overlay.send", "overlay packet not sent (rc=%d errno=%d)", (int)sent, errno);
    else if (seq == 1)
        dbg_event(DBG_TRACE, "overlay.first", "first overlay packet sent (renderer must be loaded to show it)");
}

/* ----------------------------------------------------------- diagnostics
 * Raw dumps used to find out what the sensor APIs really return on a given
 * firmware. A capture = DIAG_REPORTS consecutive reports appended to
 * <DATA_DIR>/diag.txt (size capped). Captures run at startup, ~8 s after a
 * game's memory pool first appears, and whenever <DATA_DIR>/DIAG exists.
 */
#define DIAG_REPORTS 6
#define DIAG_MAX_BYTES (96 * 1024)

static FILE *g_diag;

static void dlog(const char *fmt, ...)
{
    if (!g_diag)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_diag, fmt, ap);
    va_end(ap);
}

static int diag_begin(void)
{
    char path[160];
    struct stat st;
    snprintf(path, sizeof path, "%s/diag.txt", DATA_DIR);
    if (stat(path, &st) == 0 && st.st_size > DIAG_MAX_BYTES)
        return 0;
    g_diag = fopen(path, "a");
    return g_diag != NULL;
}

static void diag_end(void)
{
    if (g_diag)
    {
        fclose(g_diag);
        g_diag = NULL;
    }
}

#ifndef PREFBAR_SIM
static void diag_sysctl_int(const char *name)
{
    unsigned char b[16];
    size_t len = sizeof b;
    memset(b, 0, sizeof b);
    if (sysctlbyname(name, b, &len, NULL, 0) != 0)
    {
        dlog("  %s = ERR errno=%d\n", name, errno);
        return;
    }
    unsigned long long v = 0;
    for (size_t i = 0; i < len && i < 8; i++)
        v |= (unsigned long long)b[i] << (8 * i);
    dlog("  %s = %llu (len %lu)\n", name, v, (unsigned long)len);
}

static void diag_sysctl_arr(const char *name)
{
    size_t len = 0;
    if (sysctlbyname(name, NULL, &len, NULL, 0) != 0 || len == 0 || len > 2048)
    {
        dlog("  %s = ERR errno=%d len=%lu\n", name, errno, (unsigned long)len);
        return;
    }
    unsigned long a[256];
    memset(a, 0, sizeof a);
    if (sysctlbyname(name, a, &len, NULL, 0) != 0)
    {
        dlog("  %s = ERR errno=%d\n", name, errno);
        return;
    }
    dlog("  %s =", name);
    for (size_t i = 0; i < len / sizeof(unsigned long); i++)
        dlog(" %lu", a[i]);
    dlog("\n");
}
#endif

static void diag_capture(const char *tag, int idx, uint64_t t_ns, uint64_t t0_ns,
                         const cpuu_t *c, const sensors_t *s)
{
    if (!diag_begin())
        return;
    dlog("\n=== capture \"%s\" #%d  t=%.3fs ===\n", tag, idx, (double)(t_ns - t0_ns) / 1e9);

    /* 1. raw idle-thread times (this is what the CPU-usage maths consumes) */
    dlog("cpu: threads=%d idle_mask=0x%x found=0x%x\n", c->raw_nthreads, c->idle_mask, c->raw_found);
    for (int core = 0; core < NCORES; core++)
    {
        if (!(c->raw_found & (1 << core)))
            continue;
        dlog("  core%d tid=%u user=%lld.%09lld sys=%lld.%09lld\n", core, c->idle_tid[core],
             (long long)c->raw_user[core].sec, (long long)c->raw_user[core].nsec,
             (long long)c->raw_sys[core].sec, (long long)c->raw_sys[core].nsec);
    }
    { /* top 5 threads by cumulative CPU time, with names */
        int top[5] = {-1, -1, -1, -1, -1};
        double tv[5] = {-1, -1, -1, -1, -1};
        int n = c->raw_nthreads;
        if (n > MAX_THREADS)
            n = MAX_THREADS;
        for (int i = 0; i < n; i++)
        {
            double v = ts_sec(g_threads[i].user) + ts_sec(g_threads[i].sys);
            for (int k = 0; k < 5; k++)
            {
                if (v > tv[k])
                {
                    for (int m = 4; m > k; m--)
                    {
                        tv[m] = tv[m - 1];
                        top[m] = top[m - 1];
                    }
                    tv[k] = v;
                    top[k] = i;
                    break;
                }
            }
        }
        for (int k = 0; k < 5; k++)
        {
            if (top[k] < 0)
                continue;
            char name[0x40];
            memset(name, 0, sizeof name);
            if (SYM_PRESENT(sceKernelGetThreadName))
                sceKernelGetThreadName(g_threads[top[k]].td_tid, name);
            dlog("  top%d tid=%u name=\"%s\" total=%.3fs user=%lld.%09lld sys=%lld.%09lld\n",
                 k + 1, g_threads[top[k]].td_tid, name, tv[k],
                 (long long)g_threads[top[k]].user.sec, (long long)g_threads[top[k]].user.nsec,
                 (long long)g_threads[top[k]].sys.sec, (long long)g_threads[top[k]].sys.nsec);
        }
    }

    /* 2. fan duty API outputs (the API does not expose tachometer RPM here) */
    dlog("fan: called=%d rc=%d duty_q10=%u chassis_info=0x%016llx duty_pct=%d\n",
         s->fan_called, s->fan_rc, (unsigned)s->fan_duty_q10,
         (unsigned long long)s->fan_chassis, s->have_fan ? s->fan : -1);

    /* 3. memory: the pools etaHEN labels RAM/VRAM, plus a few neighbours */
    if (SYM_PRESENT(get_page_table_stats))
    {
        static const int combo[][2] = {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {0, 1}, {0, 2}, {2, 1}, {2, 2}};
        for (unsigned i = 0; i < sizeof combo / sizeof combo[0]; i++)
        {
            int total = 0, fr = 0;
            int rc = get_page_table_stats(combo[i][0], combo[i][1], &total, &fr);
            dlog("  pts(vm=%d,type=%d) rc=%d total=%d free=%d\n", combo[i][0], combo[i][1], rc, total, fr);
        }
    }
#ifndef PREFBAR_SIM
    dlog("sysctl:\n");
    diag_sysctl_int("hw.physmem");
    diag_sysctl_int("hw.realmem");
    diag_sysctl_int("hw.usermem");
    diag_sysctl_int("hw.pagesize");
    diag_sysctl_int("hw.ncpu");
    diag_sysctl_int("vm.stats.vm.v_page_count");
    diag_sysctl_int("vm.stats.vm.v_free_count");
    diag_sysctl_int("vm.stats.vm.v_wire_count");
    diag_sysctl_int("vm.stats.vm.v_active_count");
    diag_sysctl_int("vm.stats.vm.v_inactive_count");
    diag_sysctl_arr("kern.cp_time");
    diag_sysctl_arr("kern.cp_times");
#else
    dlog("sysctl: (skipped in simulation)\n");
#endif
    diag_end();
}

/* ---------------------------------------------------------------- config */

static int load_config(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->notify_sec = read_config_int("notify_sec", 0, 0, 3600);
    c->poll_us = read_config_int("poll_us", 2000, 500, 20000);
    c->sensors = read_config_int("sensors", 1, 0, 1);
#ifdef PREFBAR_EMBED
    c->diag = read_config_int("diag", 0, 0, 1);
#else
    c->diag = read_config_int("diag", 1, 0, 1);
#endif
    c->overlay = read_config_int("overlay", 1, 0, 1);
    c->ov_mode = read_config_int("overlay_mode", 0, 0, 1);
    c->ov_font = read_config_int("overlay_font", 26, 10, 96);
    {
        const int configured_corner = read_config_int(
            "overlay_corner", PREFBAR_CORNER_TL, PREFBAR_CORNER_TL, PREFBAR_CORNER_BL);
        c->ov_corner = configured_corner == PREFBAR_CORNER_BL
                           ? PREFBAR_CORNER_BL
                           : PREFBAR_CORNER_TL;
    }
    c->ov_x = read_config_int("overlay_x", 24, 0, 1800);
    c->ov_y = read_config_int("overlay_y", 24, 0, 1000);
    c->notifications = read_config_int("notifications", 1, 0, 1);
    c->startup_notification = read_config_int("startup_notification", 1, 0, 1);
    c->error_notifications = read_config_int("error_notifications", 1, 0, 1);
    c->debug_notifications = read_config_int("debug_notifications", 1, 0, 1);
    c->text_shadow = read_config_int("text_shadow", 1, 0, 1);
    c->show_fps = read_config_int("show_fps", 1, 0, 1);
    c->show_cpu = read_config_int("show_cpu", 1, 0, 1);
    c->show_soc_temp = read_config_int("show_soc_temp", 1, 0, 1);
    c->show_fan_duty = read_config_int("show_fan_duty", 1, 0, 1);
    c->show_memory = read_config_int("show_memory", 1, 0, 1);
    c->touchpad_toggle = read_config_int("touchpad_toggle", 1, 0, 1);
    c->text_r = 0.95f;
    c->text_g = 0.95f;
    c->text_b = 0.95f;
    (void)read_config_rgb("text_color", &c->text_r, &c->text_g, &c->text_b);
    return access(PREFBAR_CONFIG_PATH, F_OK) == 0;
}

/* ------------------------------------------------------------------ main */
static volatile sig_atomic_t g_stop;
static void on_signal(int sig) __attribute__((unused));
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int prefbar_acquire_lock(void)
{
    mkdir(DATA_DIR, 0777);
    int lock = open(LOCK_PATH, O_CREAT | O_RDWR, 0600);
    if (lock < 0)
    {
        dbg_event(DBG_ERR, "lock", "cannot open lock file %s (errno %d)", LOCK_PATH, errno);
        return -1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0)
    {
        dbg_event(DBG_WARN, "lock", "already running - this instance exits");
        close(lock);
        return -1;
    }
    return lock;
}

/* install_signals = 0 when running as a thread inside another process */
static int prefbar_run(int install_signals, int lock)
{
    if (lock < 0)
        lock = prefbar_acquire_lock();
    if (lock < 0)
        return 1;
    ensure_default_config_file();
    if (install_signals)
    {
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
    }

    char stop_path[160];
    snprintf(stop_path, sizeof stop_path, "%s/STOP", DATA_DIR);
    unlink(stop_path); /* clear a stale stop request */

    config_t cfg;
    int have_conf = load_config(&cfg);
    if (!have_conf)
        dbg_event(DBG_WARN, "config.file", "prefbar.conf unavailable; using built-in defaults");
#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
    overlay_control_open();
#endif

    dce_t dce;
    memset(&dce, 0, sizeof dce);
    dce.fd = -1;

    static stats_t st; /* static: keeps the stack small */
    memset(&st, 0, sizeof st);

    static cpuu_t cpuu;
    memset(&cpuu, 0, sizeof cpuu);
    sensors_t sens;
    memset(&sens, 0, sizeof sens);
    if (cfg.sensors)
        sensors_read(&sens, &cpuu, now_ns()); /* prime CPU baseline */

    uint64_t last_count = 0;
    uint64_t next_report = now_ns() + REPORT_NS;
    uint64_t last_notify = 0;
    int fail_ticks = 0;

    /* diagnostics scheduling */
    const uint64_t t_start = now_ns();
    int diag_left = cfg.diag ? DIAG_REPORTS : 0; /* first capture: startup */
    int diag_idx = 0, ram_seen = 0;
    const char *diag_tag = "startup";
    uint64_t diag_arm_at = 0;
    char diag_trig[160];
    snprintf(diag_trig, sizeof diag_trig, "%s/DIAG", DATA_DIR);
    if (cfg.diag)
    { /* fresh log each run */
        char dp[160];
        snprintf(dp, sizeof dp, "%s/diag.txt", DATA_DIR);
        FILE *df = fopen(dp, "w");
        if (df)
        {
            fprintf(df, "PrefBar diag log (see Docs/HOW_IT_WORKS.md)\n");
            fclose(df);
        }
    }
#ifdef PREFBAR_SIM
    uint64_t sim_end = now_ns() + 6ull * 1000000000ull;
#endif

#ifdef PREFBAR_EMBED
    dbg_event(DBG_INFO, "start", "v%s started (embedded sampler) overlay=%s sensors=%s",
              PREFBAR_VERSION, cfg.overlay ? "on" : "off", cfg.sensors ? "on" : "off");
#else
    dbg_event(DBG_INFO, "start", "v%s started (standalone) overlay=%s sensors=%s",
              PREFBAR_VERSION, cfg.overlay ? "on" : "off", cfg.sensors ? "on" : "off");
#endif
    dbg_event(DBG_TRACE, "config", "%s: poll=%dus mode=%d font=%d diag=%d notify_sec=%d",
              have_conf ? "prefbar.conf loaded" : "no prefbar.conf, defaults",
              cfg.poll_us, cfg.ov_mode, cfg.ov_font, cfg.diag, cfg.notify_sec);

    int dce_state = -1;               /* -1 unknown, 0 failing, 1 ok          */
    int fps_active = 0, fps_seen = 0; /* frames flowing / ever seen           */
    int reports = 0, ram_noted = 0, cpu_zero_run = 0;
    int game_was_active = 0;

    while (!g_stop)
    {
#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
        overlay_control_poll();
#endif
        uint64_t t = now_ns();
        uint64_t count;
        int game_active = 1;
#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
        game_active = prefbar_game_process_ready() != 0;
#endif

        if (!game_active)
        {
            /*
             * The PS5 home screen and other system UI states commonly have
             * no /dev/dce device at all (open() returns ENOENT). That is not
             * an FPS failure: don't poll DCE or emit the 4-second error until
             * the controller has confirmed a stable game process.
             * Keep the sensor/report thread alive, but discard frame history
             * and close any stale DCE handle when leaving a game.
             */
            if (game_was_active)
            {
                memset(&st, 0, sizeof st);
                last_count = 0;
                fps_active = 0;
            }
            if (dce.fd >= 0)
            {
                close(dce.fd);
                dce.fd = -1;
            }
            dce.abi = 0;
            dce_state = -1;
            fail_ticks = 0;
        }
        else
        {
            /* Start each game with a clean baseline; don't mix counter history. */
            if (!game_was_active)
            {
                memset(&st, 0, sizeof st);
                last_count = 0;
                fail_ticks = 0;
                dce_state = -1;
            }

            if (dce_read(&dce, &count) == 0)
            {
                if (dce_state != 1)
                {
                    dce_state = 1;
                    dbg_event(DBG_TRACE, "dce.ok", "display counter readable (/dev/dce, abi=%s)",
                              dce.abi == 2 ? "sign-ext" : "zero-ext");
                }
                fail_ticks = 0;
                last_count = count;
                stats_update(&st, count, t);
            }
            else
            {
                if (dce_state != 0)
                {
                    dce_state = 0;
                    dbg_event(DBG_TRACE, "dce.fail1", "/dev/dce read failed (errno %d)", dce.last_errno);
                }
                if (++fail_ticks == 2000)
                { /* ~4 s of failure during a confirmed game */
                    write_file_atomic("status.txt",
                                      "PrefBar: cannot read /dev/dce during an active game\n");
                    dbg_event(DBG_ERR, "dce.fail", "cannot read /dev/dce for ~4 s during active game (errno %d) - no FPS", dce.last_errno);
                    fail_ticks = 0;
                }
            }
        }
        game_was_active = game_active;

        if (t >= next_report)
        {
            dbg_startup_tick();
            next_report = t + REPORT_NS;
            report_t r = stats_report(&st, last_count, t);
            if (cfg.sensors)
                sensors_read(&sens, &cpuu, t);
            reports++;

            if ((!r.idle) != fps_active)
            {
                fps_active = !r.idle;
                if (fps_active)
                {
                    dbg_event(fps_seen ? DBG_TRACE : DBG_INFO, fps_seen ? "fps.active" : "fps.first",
                              "frames flowing: %.1f FPS (%.1f ms)", r.fps, r.avg_ms);
                    fps_seen = 1;
                }
                else
                {
                    dbg_event(DBG_TRACE, "fps.idle", "no new frames (static screen or game closed)");
                }
            }
            if (cfg.sensors)
            {
                if (reports == 2) /* CPU usage needs two samples to exist */
                    dbg_event(DBG_INFO, "sensors", "sensors: cpu%%=%s cpuT=%s socT=%s fan=%s",
                              sens.have_cpu ? "y" : "n", sens.have_cpu_temp ? "y" : "n",
                              sens.have_soc_temp ? "y" : "n", sens.have_fan ? "y" : "n");
                if (reports == 2)
                    dbg_event(DBG_TRACE, "fan.rpm.unavailable",
                              "fan RPM is not exposed by the verified API; overlay shows measured fan-duty percent instead");
                if ((sens.have_ram || sens.have_vram) && !ram_noted)
                {
                    ram_noted = 1;
                    dbg_event(DBG_TRACE, "sensors.ram", "RAM/VRAM readings now available (%s%s)",
                              sens.have_ram ? "ram" : "", sens.have_vram ? " vram" : "");
                }
                if (fps_active && sens.have_cpu && sens.cpu_avg < 0.05f)
                {
                    if (++cpu_zero_run == 10)
                        dbg_event(DBG_TRACE, "cpu.zero", "CPU usage reads 0%% in game (known issue; see Docs/HOW_IT_WORKS.md)");
                }
                else
                {
                    cpu_zero_run = 0;
                }
            }

            if (cfg.diag && cfg.sensors)
            {
                if (!sens.have_ram)
                    ram_seen = 0;
                if (diag_left == 0)
                {
                    if (access(diag_trig, F_OK) == 0)
                    {
                        unlink(diag_trig);
                        diag_left = DIAG_REPORTS;
                        diag_idx = 0;
                        diag_tag = "trigger";
                    }
                    else if (sens.have_ram && !ram_seen)
                    {
                        ram_seen = 1;
                        diag_arm_at = t + 8ull * 1000000000ull;
                    }
                    else if (diag_arm_at && t >= diag_arm_at)
                    {
                        diag_arm_at = 0;
                        diag_left = DIAG_REPORTS;
                        diag_idx = 0;
                        diag_tag = "in-game";
                    }
                }
                if (diag_left > 0)
                {
                    diag_capture(diag_tag, ++diag_idx, t, t_start, &cpuu, &sens);
                    diag_left--;
                }
            }

            char l1[192], l2[320], l3[160], txt[720], kv[1024];
            format_perf(&r, l1, sizeof l1);
            l2[0] = l3[0] = 0;
            if (cfg.sensors)
            {
                format_sensors(&sens, l2, sizeof l2);
                format_cores(&sens, l3, sizeof l3);
            }
            snprintf(txt, sizeof txt, "%s\n", l1);
            if (l2[0])
                app(txt, sizeof txt, "%s\n", l2);
            if (l3[0])
                app(txt, sizeof txt, "%s\n", l3);
            write_file_atomic("status.txt", txt);

            format_kv(&r, &sens, kv, sizeof kv);
            write_file_atomic("status.kv", kv);
            {
                char orows[PREFBAR_ROWS][PREFBAR_ROW_LEN];
                int on = build_rows(&cfg, &r, &sens, orows);
                overlay_send(&cfg, orows, on);
            }
#ifdef PREFBAR_SIM
            printf("%s", txt);
            printf("--\n");
#endif
            if (cfg.notifications && cfg.debug_notifications && cfg.notify_sec > 0 && !r.idle &&
                (t - last_notify) >= (uint64_t)cfg.notify_sec * 1000000000ull)
            {
                char toast[512];
                snprintf(toast, sizeof toast, "%s", l1);
                if (l2[0])
                    app(toast, sizeof toast, "\n%s", l2);
                notify(toast);
                last_notify = t;
            }
            if (access(stop_path, F_OK) == 0)
            {
                unlink(stop_path);
                dbg_event(DBG_INFO, "stop", "STOP file found - stopping");
                break;
            }
        }
#ifdef PREFBAR_SIM
        if (t >= sim_end)
            break;
#endif
        usleep((useconds_t)cfg.poll_us);
    }

    dbg_event(DBG_INFO, "exit", "sampler stopped");
#if defined(PREFBAR_EMBED) && !defined(PREFBAR_SIM)
    if (g_control_fd >= 0)
    {
        close(g_control_fd);
        g_control_fd = -1;
    }
#endif
    if (dce.fd >= 0)
        close(dce.fd);
    if (install_signals)
        close(lock);
    return 0;
}

#ifdef PREFBAR_EMBED
static void *prefbar_thread(void *arg)
{
    (void)prefbar_run(0, (int)(intptr_t)arg);
    return NULL;
}

/* Starts PrefBar as a detached thread inside the overlay controller process.
 * The singleton lock is checked synchronously and held for the process lifetime.
 * No signal handlers are installed; stop the sampler with <DATA_DIR>/STOP.
 * Returns 0 on success. */
int prefbar_telemetry_embedded_start(void)
{
    int lock = prefbar_acquire_lock();
    if (lock < 0)
        return 1;
    pthread_t th;
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0)
    {
        close(lock);
        return -1;
    }
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    int rc = pthread_create(&th, &at, prefbar_thread, (void *)(intptr_t)lock);
    pthread_attr_destroy(&at);
    if (rc != 0)
    {
        close(lock);
        dbg_event(DBG_ERR, "thread", "sampler thread failed to start (rc=%d): no FPS/sensors", rc);
    }
    return rc;
}
#else
int main(void)
{
    return prefbar_run(1, -1);
}
#endif
