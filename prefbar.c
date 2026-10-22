// SPDX-License-Identifier: GPL-3.0-or-later
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
#ifdef PERFBAR_EMBED
#include <pthread.h>
#include "common_fps/perf_wire.h"
#else
#include "perf_wire.h"
#endif
#ifndef PERFBAR_SIM
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#ifdef PERFBAR_SIM
#define DATA_DIR  "/tmp/perfbar"
#define LOCK_PATH "/tmp/perfbar.lock"
#else
#define DATA_DIR  "/data/perfbar"
#define LOCK_PATH "/system_tmp/perfbar.lock"
#endif

#define HIST          600
#define MAX_FLIPS_PER_POLL 8
#define REPORT_NS     1000000000ull
#define STALE_NS      2000000000ull
#define NCORES        8
#define MAX_THREADS   3072

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifndef PERFBAR_SIM

#define DCE_IOCTL_ZERO 0x0000000080308217UL
#define DCE_IOCTL_SIGN 0xFFFFFFFF80308217UL
#define DCE_ARG0       0x10000000AULL
#define DCE_ARG1       0x8000000000ULL
#define DCE_OUT_BYTES  0x60
#define DCE_COUNT_OFF  8

struct dce_arg {
    uint64_t selector;
    uint64_t mask;
    uint64_t output;
    uint64_t reserved[3];
};
typedef char dce_arg_size_check[(sizeof(struct dce_arg) == 0x30) ? 1 : -1];

typedef struct {
    int fd;
    int abi;
    int last_errno;
} dce_t;

static int dce_open(dce_t *d)
{
    if (d->fd >= 0) return 0;
    d->fd = open("/dev/dce", O_RDWR);
    if (d->fd < 0) { d->last_errno = errno; return -1; }
    return 0;
}

static int dce_ioctl_once(dce_t *d, unsigned long req, unsigned char *out)
{
    struct dce_arg a;
    memset(out, 0, DCE_OUT_BYTES);
    memset(&a, 0, sizeof a);
    a.selector = DCE_ARG0;
    a.mask     = DCE_ARG1;
    a.output   = (uint64_t)(uintptr_t)out;
    return ioctl(d->fd, req, &a);
}

static int dce_read(dce_t *d, uint64_t *count)
{
    unsigned char out[DCE_OUT_BYTES];
    if (dce_open(d) != 0) return -1;

    unsigned long req = (d->abi == 2) ? DCE_IOCTL_SIGN : DCE_IOCTL_ZERO;
    int rc = dce_ioctl_once(d, req, out);
    int err = rc < 0 ? errno : 0;

    if (rc < 0 && err == EINVAL && req != DCE_IOCTL_SIGN) {
        req = DCE_IOCTL_SIGN;
        rc = dce_ioctl_once(d, req, out);
        err = rc < 0 ? errno : 0;
    }
    if (rc < 0) {
        d->last_errno = err;
        if (err == EBADF) { close(d->fd); d->fd = -1; }
        return -1;
    }
    d->abi = (req == DCE_IOCTL_SIGN) ? 2 : 1;
    memcpy(count, out + DCE_COUNT_OFF, sizeof *count);
    return 0;
}

#else

typedef struct { int fd; int abi; int last_errno; uint64_t t0; } dce_t;
#define SIM_FRAMES 4000

static int dce_read(dce_t *d, uint64_t *count)
{
    static double stamp[SIM_FRAMES];
    static int init;
    if (!init) {
        double t = 0.0;
        for (int i = 0; i < SIM_FRAMES; i++) {
            t += 1000.0 / 60.0 + ((i % 50 == 49) ? 20.0 : 0.0);
            stamp[i] = t;
        }
        init = 1;
    }
    if (!d->t0) d->t0 = now_ns();
    double ms = (double)(now_ns() - d->t0) / 1e6;
    uint64_t n = 0;
    while (n < SIM_FRAMES && stamp[n] <= ms) n++;
    *count = 1000 + n;
    return 0;
}
#endif

typedef struct { int64_t sec, nsec; } ps_ts;
struct ps_proc_stats {
    int32_t  lo_data;
    uint32_t td_tid;
    ps_ts    user;
    ps_ts    sys;
};
typedef char ps_stats_size_check[(sizeof(struct ps_proc_stats) == 0x28) ? 1 : -1];

#ifndef PERFBAR_SIM
extern int sceKernelGetCpuTemperature(int *) __attribute__((weak));
extern int sceKernelGetSocSensorTemperature(int, int *) __attribute__((weak));
extern int sceKernelGetCurrentFanDuty(int *, int *) __attribute__((weak));
extern int sceKernelGetCpuUsage(struct ps_proc_stats *, int32_t *) __attribute__((weak));
extern int sceKernelGetThreadName(uint32_t, char *) __attribute__((weak));
extern int get_page_table_stats(int, int, int *, int *) __attribute__((weak));
#define SYM_PRESENT(f) ((void *)(f) != NULL)
#else
#define SYM_PRESENT(f) 1
static double sim_elapsed(void)
{
    static uint64_t t0;
    if (!t0) t0 = now_ns();
    return (double)(now_ns() - t0) / 1e9;
}
static int sceKernelGetCpuTemperature(int *t) { *t = 60 + (int)sim_elapsed(); return 0; }
static int sceKernelGetSocSensorTemperature(int id, int *t)
{ (void)id; *t = 55 + (int)sim_elapsed(); return 0; }
static int sceKernelGetCurrentFanDuty(int *u, int *d) { (void)u; (void)d; return -1; }
static const double SIM_IDLE_FRAC[NCORES] = {0.90, 0.70, 0.50, 0.80, 0.60, 0.95, 0.70, 0.85};
static int sceKernelGetThreadName(uint32_t id, char *out)
{
    if (id >= 100 && id < 100 + NCORES) snprintf(out, 0x40, "SceIdleCpu%u", id - 100);
    else snprintf(out, 0x40, "SceWorker%u", id);
    return 0;
}
static int sceKernelGetCpuUsage(struct ps_proc_stats *out, int32_t *n)
{
    double e = sim_elapsed();
    int count = 24;
    for (int i = 0; i < count; i++) {
        double v = (i < NCORES) ? e * SIM_IDLE_FRAC[i] : e * 0.1;
        out[i].lo_data = 0;
        out[i].td_tid = (uint32_t)(100 + i);
        out[i].user.sec = 0; out[i].user.nsec = 0;
        out[i].sys.sec = (int64_t)v;
        out[i].sys.nsec = (int64_t)((v - (double)(int64_t)v) * 1e9);
    }
    *n = count;
    return 0;
}
static int get_page_table_stats(int vm, int type, int *total, int *fr)
{
    (void)vm;
    if (type == 1) { *total = 12288; *fr = 4096 + ((int)(sim_elapsed() * 100) % 512); }
    else           { *total = 4096;  *fr = 1024; }
    return 0;
}
#endif

typedef struct {
    float    ft_ms[HIST];
    int      head, count;
    uint64_t last_flip_ns;
    uint64_t last_count;
    int      have_count;
    uint64_t win_start_ns, win_start_count;
} stats_t;

static void stats_push(stats_t *s, float ms)
{
    s->ft_ms[s->head] = ms;
    s->head = (s->head + 1) % HIST;
    if (s->count < HIST) s->count++;
}

static void stats_update(stats_t *s, uint64_t count, uint64_t t_ns)
{
    if (!s->have_count) {
        s->have_count = 1;
        s->last_count = count;
        s->last_flip_ns = t_ns;
        s->win_start_ns = t_ns;
        s->win_start_count = count;
        return;
    }
    if (count < s->last_count) {
        s->last_count = count;
        s->last_flip_ns = t_ns;
        s->win_start_ns = t_ns;
        s->win_start_count = count;
        return;
    }
    uint64_t flips = count - s->last_count;
    if (flips) {
        if (flips > MAX_FLIPS_PER_POLL) flips = MAX_FLIPS_PER_POLL;
        float ms = (float)(t_ns - s->last_flip_ns) / 1e6f / (float)flips;
        for (uint64_t i = 0; i < flips; i++) stats_push(s, ms);
        s->last_count = count;
        s->last_flip_ns = t_ns;
    }
}

static int cmp_desc(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) - (x > y);
}

typedef struct {
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
    if (s->count >= 2) {
        float tmp[HIST];
        double sum = 0.0;
        memcpy(tmp, s->ft_ms, sizeof(float) * (size_t)s->count);
        for (int i = 0; i < s->count; i++) sum += tmp[i];
        r.avg_ms = sum / s->count;
        for (int i = 0; i < s->count; i++)
            if (tmp[i] > 2.0 * r.avg_ms && tmp[i] > 20.0f) r.hitches++;
        qsort(tmp, (size_t)s->count, sizeof(float), cmp_desc);
        int k = s->count / 100; if (k < 1) k = 1;
        double worst = 0.0;
        for (int i = 0; i < k; i++) worst += tmp[i];
        worst /= k;
        r.low1_fps = worst > 0.0 ? 1000.0 / worst : 0.0;
        r.max_ms = tmp[0];
    }
    return r;
}

typedef struct {
    int   have_cpu_temp, cpu_temp;
    int   have_soc_temp, soc_temp;
    int   have_fan, fan;
    int   have_cpu, ncores_known;
    float core[NCORES], cpu_avg;
    int   core_ok[NCORES];
    int   have_ram, ram_used_mb, ram_total_mb; float ram_pct;
    int   have_vram; float vram_pct; int vram_used_mb, vram_total_mb;
    int   fan_called, fan_rc, fan_unk, fan_raw;
    int   dbg_have; double dbg_dt; float dbg_ratio[NCORES];
} sensors_t;

typedef struct {
    uint32_t idle_tid[NCORES];
    int      idle_mask;
    int      scanned;
    double   prev_idle_s[NCORES];
    uint64_t prev_t_ns;
    int      have_prev;
    ps_ts    raw_user[NCORES], raw_sys[NCORES];
    int      raw_found, raw_nthreads;
} cpuu_t;

static struct ps_proc_stats g_threads[MAX_THREADS];

static double ts_sec(ps_ts t) { return (double)t.sec + (double)t.nsec / 1e9; }

static void cpu_usage_sample(cpuu_t *c, sensors_t *s, uint64_t t_ns)
{
    if (!SYM_PRESENT(sceKernelGetCpuUsage)) return;
    int32_t n = MAX_THREADS;
    if (sceKernelGetCpuUsage(g_threads, &n) != 0 || n <= 0) return;
    if (n > MAX_THREADS) n = MAX_THREADS;

    if (!c->scanned && SYM_PRESENT(sceKernelGetThreadName)) {
        for (int i = 0; i < n; i++) {
            char name[0x40];
            int core = -1;
            memset(name, 0, sizeof name);
            if (sceKernelGetThreadName(g_threads[i].td_tid, name) == 0 &&
                sscanf(name, "SceIdleCpu%d", &core) == 1 &&
                core >= 0 && core < NCORES) {
                c->idle_tid[core] = g_threads[i].td_tid;
                c->idle_mask |= 1 << core;
            }
        }
        c->scanned = 1;
    }
    if (!c->idle_mask) return;

    double idle_s[NCORES] = {0};
    int found = 0;
    for (int core = 0; core < NCORES; core++) {
        if (!(c->idle_mask & (1 << core))) continue;
        for (int i = 0; i < n; i++) {
            if (g_threads[i].td_tid == c->idle_tid[core]) {
                idle_s[core] = ts_sec(g_threads[i].user) + ts_sec(g_threads[i].sys);
                c->raw_user[core] = g_threads[i].user;
                c->raw_sys[core]  = g_threads[i].sys;
                found |= 1 << core;
                break;
            }
        }
    }

    c->raw_found = found;
    c->raw_nthreads = n;
    if (c->have_prev && t_ns > c->prev_t_ns) {
        double dt = (double)(t_ns - c->prev_t_ns) / 1e9;
        double sum = 0.0; int cnt = 0;
        for (int core = 0; core < NCORES; core++) {
            s->core_ok[core] = 0;
            if (!(found & (1 << core))) continue;
            double idle = (idle_s[core] - c->prev_idle_s[core]) / dt;
            s->dbg_ratio[core] = (float)idle;
            s->dbg_dt = dt; s->dbg_have = 1;
            if (idle > 1.0) idle = 1.0;
            if (idle < 0.0) idle = 0.0;
            s->core[core] = (float)((1.0 - idle) * 100.0);
            s->core_ok[core] = 1;
            sum += s->core[core]; cnt++;
        }
        if (cnt) { s->cpu_avg = (float)(sum / cnt); s->ncores_known = cnt; s->have_cpu = 1; }
    }
    for (int core = 0; core < NCORES; core++) c->prev_idle_s[core] = idle_s[core];
    c->prev_t_ns = t_ns;
    c->have_prev = 1;
}

static void sensors_read(sensors_t *s, cpuu_t *c, uint64_t t_ns)
{
    memset(s, 0, sizeof *s);

    int v = 0;
    if (SYM_PRESENT(sceKernelGetCpuTemperature) && sceKernelGetCpuTemperature(&v) == 0 && v > 0)
        { s->have_cpu_temp = 1; s->cpu_temp = v; }
    v = 0;
    if (SYM_PRESENT(sceKernelGetSocSensorTemperature) && sceKernelGetSocSensorTemperature(0, &v) == 0 && v > 0)
        { s->have_soc_temp = 1; s->soc_temp = v; }

    int unk = 0, duty = 0;
    if (SYM_PRESENT(sceKernelGetCurrentFanDuty)) {
        int rc = sceKernelGetCurrentFanDuty(&unk, &duty);
        s->fan_called = 1; s->fan_rc = rc; s->fan_unk = unk; s->fan_raw = duty;
        if (rc == 0 && duty >= 0 && duty <= 100) { s->have_fan = 1; s->fan = duty; }
    }

    cpu_usage_sample(c, s, t_ns);

    int total = 0, fr = 0;
    if (SYM_PRESENT(get_page_table_stats) && get_page_table_stats(1, 1, &total, &fr) != -1 && total > 0) {
        s->have_ram = 1; s->ram_total_mb = total; s->ram_used_mb = total - fr;
        s->ram_pct = 100.0f * (float)(total - fr) / (float)total;
    }
    total = 0; fr = 0;
    if (SYM_PRESENT(get_page_table_stats) && get_page_table_stats(1, 2, &total, &fr) != -1 && total > 0) {
        s->have_vram = 1;
        s->vram_total_mb = total; s->vram_used_mb = total - fr;
        s->vram_pct = 100.0f * (float)(total - fr) / (float)total;
    }
}

static void app(char *buf, size_t n, const char *fmt, ...)
{
    size_t len = strlen(buf);
    if (len >= n - 1) return;
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
    if (s->have_cpu) {
        app(buf, n, "%sCPU %.0f%%", sep, s->cpu_avg);
        if (s->have_cpu_temp) app(buf, n, " %dC", s->cpu_temp);
        sep = " | ";
    } else if (s->have_cpu_temp) {
        app(buf, n, "%sCPU %dC", sep, s->cpu_temp); sep = " | ";
    }
    if (s->have_soc_temp) { app(buf, n, "%sSoC %dC", sep, s->soc_temp); sep = " | "; }
    if (s->have_fan)      { app(buf, n, "%sFan %d", sep, s->fan); sep = " | "; }
    if (s->have_ram) {
        app(buf, n, "%sRAM %d/%d MB (%.0f%%)", sep, s->ram_used_mb, s->ram_total_mb, s->ram_pct);
        sep = " | ";
    }
    if (s->have_vram) { app(buf, n, "%sVRAM %.0f%% (%d/%d MB)", sep, s->vram_pct, s->vram_used_mb, s->vram_total_mb); sep = " | "; }
    if (!*sep) app(buf, n, "sensors: none available");
}

static void format_cores(const sensors_t *s, char *buf, size_t n)
{
    buf[0] = 0;
    if (!s->have_cpu) return;
    app(buf, n, "Cores %%:");
    for (int i = 0; i < NCORES; i++) {
        if (s->core_ok[i]) app(buf, n, " %.0f", s->core[i]);
        else               app(buf, n, " --");
    }
}

static void format_kv(const report_t *r, const sensors_t *s, char *buf, size_t n)
{
    buf[0] = 0;
    app(buf, n, "ts=%lld\n", (long long)time(NULL));
    app(buf, n, "idle=%d\n", r->idle);
    if (!r->idle) {
        app(buf, n, "fps=%.1f\nframetime_ms=%.2f\nlow1_fps=%.1f\nmax_ms=%.2f\nhitches=%d\n",
            r->fps, r->avg_ms, r->low1_fps, r->max_ms, r->hitches);
    }
    if (s->have_cpu) {
        app(buf, n, "cpu_avg=%.1f\n", s->cpu_avg);
        for (int i = 0; i < NCORES; i++)
            if (s->core_ok[i]) app(buf, n, "cpu%d=%.1f\n", i, s->core[i]);
    }
    if (s->have_cpu_temp) app(buf, n, "cpu_temp_c=%d\n", s->cpu_temp);
    if (s->have_soc_temp) app(buf, n, "soc_temp_c=%d\n", s->soc_temp);
    if (s->have_fan)      app(buf, n, "fan_duty=%d\n", s->fan);
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
    if (!f) return;
    fputs(text, f);
    fclose(f);
    rename(tmp, dst);
}

typedef struct {
    int notify_sec, poll_us, sensors, diag;
    int overlay, ov_mode, ov_font, ov_corner, ov_x, ov_y;
} config_t;

static int g_udp = -1;

static int build_rows(const config_t *cfg, const report_t *r, const sensors_t *s,
                      char rows[PERF_ROWS][PERF_ROW_LEN])
{
    int n = 0;
    char *l;

    l = rows[n]; l[0] = 0;
    if (r->idle) app(l, PERF_ROW_LEN, "FPS --");
    else app(l, PERF_ROW_LEN, "FPS %.1f  %.1f ms  1%%L %.1f", r->fps, r->avg_ms, r->low1_fps);
    n++;

    if (cfg->sensors) {
        l = rows[n]; l[0] = 0;
        if (s->have_cpu) {
            app(l, PERF_ROW_LEN, "CPU %.0f%%", s->cpu_avg);
            if (s->have_cpu_temp) app(l, PERF_ROW_LEN, " %dC", s->cpu_temp);
        } else if (s->have_cpu_temp) {
            app(l, PERF_ROW_LEN, "CPU %dC", s->cpu_temp);
        }
        if (s->have_soc_temp) app(l, PERF_ROW_LEN, "%sSoC %dC", l[0] ? "  " : "", s->soc_temp);
        if (l[0]) n++;

        l = rows[n]; l[0] = 0;
        if (s->have_ram)  app(l, PERF_ROW_LEN, "RAM %d/%d MB", s->ram_used_mb, s->ram_total_mb);
        if (s->have_vram) app(l, PERF_ROW_LEN, "%sVRAM %.0f%%", l[0] ? "  " : "", s->vram_pct);
        if (l[0]) n++;
    }

    if (cfg->ov_mode == 1 && cfg->sensors) {
        if (n < PERF_ROWS && s->have_cpu) {
            l = rows[n]; l[0] = 0;
            app(l, PERF_ROW_LEN, "cores:");
            for (int i = 0; i < NCORES; i++) {
                if (s->core_ok[i]) app(l, PERF_ROW_LEN, " %.0f", s->core[i]);
                else               app(l, PERF_ROW_LEN, " --");
            }
            n++;
        }
        if (n < PERF_ROWS && s->dbg_have) {
            l = rows[n]; l[0] = 0;
            app(l, PERF_ROW_LEN, "idle/dt:");
            for (int i = 0; i < NCORES; i++) app(l, PERF_ROW_LEN, " %.2f", (double)s->dbg_ratio[i]);
            n++;
        }
        if (n < PERF_ROWS) {
            l = rows[n]; l[0] = 0;
            if (s->fan_called) app(l, PERF_ROW_LEN, "fan 0x%08x", (unsigned)s->fan_raw);
            if (s->have_vram)  app(l, PERF_ROW_LEN, "%sVRAM %d/%d MB", l[0] ? "  " : "", s->vram_used_mb, s->vram_total_mb);
            app(l, PERF_ROW_LEN, "%shit %d", l[0] ? "  " : "", r->hitches);
            n++;
        }
    }
    int target = cfg->sensors ? (cfg->ov_mode == 1 ? PERF_ROWS : 3) : 1;
    while (n < target) { rows[n][0] = 0; n++; }
    return n;
}

static void overlay_send(const config_t *cfg, char rows[PERF_ROWS][PERF_ROW_LEN], int n)
{
    static uint64_t seq;
    if (n < 1) return;
    if (g_udp < 0) g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0) return;

    perf_wire_t p;
    memset(&p, 0, sizeof p);
    p.magic = PERF_WIRE_MAGIC;
    p.version = PERF_WIRE_VERSION;
    p.size = (uint16_t)sizeof p;
    p.sequence = ++seq;
    p.corner = (uint8_t)cfg->ov_corner;
    p.rows = (uint8_t)n;
    p.font_size = (uint16_t)cfg->ov_font;
    p.margin_x = (float)cfg->ov_x;
    p.margin_y = (float)cfg->ov_y;
    p.r = 0.95f; p.g = 0.95f; p.b = 0.95f; p.a = 1.0f;
    for (int i = 0; i < n; i++) {
        size_t len = strnlen(rows[i], PERF_ROW_LEN - 1);
        memcpy(p.text[i], rows[i], len);
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PERF_WIRE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)sendto(g_udp, &p, sizeof p, 0, (struct sockaddr *)&a, sizeof a);
}

#define DIAG_REPORTS   6
#define DIAG_MAX_BYTES (96 * 1024)

static FILE *g_diag;

static void dlog(const char *fmt, ...)
{
    if (!g_diag) return;
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
    if (stat(path, &st) == 0 && st.st_size > DIAG_MAX_BYTES) return 0;
    g_diag = fopen(path, "a");
    return g_diag != NULL;
}

static void diag_end(void)
{
    if (g_diag) { fclose(g_diag); g_diag = NULL; }
}

static void diag_capture(const char *tag, int idx, uint64_t t_ns, uint64_t t0_ns,
                         const cpuu_t *c, const sensors_t *s)
{
    if (!diag_begin()) return;
    dlog("\n=== capture \"%s\" #%d  t=%.3fs ===\n", tag, idx, (double)(t_ns - t0_ns) / 1e9);
    dlog("cpu: threads=%d idle_mask=0x%x found=0x%x\n", c->raw_nthreads, c->idle_mask, c->raw_found);
    dlog("fan: called=%d rc=%d out_unk=0x%08x out_duty=0x%08x (%d)\n",
         s->fan_called, s->fan_rc, (unsigned)s->fan_unk, (unsigned)s->fan_raw, s->fan_raw);
    diag_end();
}

#if defined(PERFBAR_NOTIFY) && !defined(PERFBAR_SIM)
typedef struct {
    int32_t type; int32_t req_id; int32_t priority; int32_t msg_id;
    int32_t target_id; int32_t user_id; int32_t unk1; int32_t unk2;
    int32_t app_id; int32_t error_num; int32_t unk3; char use_icon_image_uri;
    char message[1024]; char uri[1024]; char unkstr[1024];
} notify_req_t;
extern int sceKernelSendNotificationRequest(int, notify_req_t *, size_t, int);

static void notify(const char *msg)
{
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

static void load_config(config_t *c)
{
    c->notify_sec = 0; c->poll_us = 2000; c->sensors = 1;
#ifdef PERFBAR_EMBED
    c->diag = 0;
#else
    c->diag = 1;
#endif
    c->overlay = 1; c->ov_mode = 0; c->ov_font = 26; c->ov_corner = PERF_CORNER_TL; c->ov_x = 24; c->ov_y = 24;
    char path[160];
    snprintf(path, sizeof path, "%s/perfbar.conf", DATA_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "notify_sec=%d", &v) == 1 && v >= 0 && v <= 3600) c->notify_sec = v;
        if (sscanf(line, "poll_us=%d", &v) == 1 && v >= 500 && v <= 20000) c->poll_us = v;
        if (sscanf(line, "sensors=%d", &v) == 1 && (v == 0 || v == 1)) c->sensors = v;
        if (sscanf(line, "diag=%d", &v) == 1 && (v == 0 || v == 1)) c->diag = v;
        if (sscanf(line, "overlay=%d", &v) == 1 && (v == 0 || v == 1)) c->overlay = v;
        if (sscanf(line, "overlay_mode=%d", &v) == 1 && (v == 0 || v == 1)) c->ov_mode = v;
    }
    fclose(f);
}

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int perfbar_run(int install_signals)
{
    mkdir(DATA_DIR, 0777);
    int lock = open(LOCK_PATH, O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        notify("perfbar: already running");
        return 1;
    }
    if (install_signals) {
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
    }

    char stop_path[160];
    snprintf(stop_path, sizeof stop_path, "%s/STOP", DATA_DIR);
    unlink(stop_path);

    config_t cfg;
    load_config(&cfg);

    dce_t dce;
    memset(&dce, 0, sizeof dce);
    dce.fd = -1;

    static stats_t st;
    memset(&st, 0, sizeof st);

    static cpuu_t cpuu;
    memset(&cpuu, 0, sizeof cpuu);
    sensors_t sens;
    memset(&sens, 0, sizeof sens);
    if (cfg.sensors) sensors_read(&sens, &cpuu, now_ns());

    uint64_t last_count = 0;
    uint64_t next_report = now_ns() + REPORT_NS;
    uint64_t last_notify = 0;
    int fail_ticks = 0;

    notify("perfbar started");

    while (!g_stop) {
        uint64_t t = now_ns();
        uint64_t count;
        if (dce_read(&dce, &count) == 0) {
            fail_ticks = 0;
            last_count = count;
            stats_update(&st, count, t);
        } else if (++fail_ticks == 2000) {
            write_file_atomic("status.txt", "perfbar: cannot read /dev/dce (not jailbroken / sandboxed?)\n");
            fail_ticks = 0;
        }

        if (t >= next_report) {
            next_report = t + REPORT_NS;
            report_t r = stats_report(&st, last_count, t);
            if (cfg.sensors) sensors_read(&sens, &cpuu, t);

            char l1[192], l2[320], l3[160], txt[720], kv[1024];
            format_perf(&r, l1, sizeof l1);
            l2[0] = l3[0] = 0;
            if (cfg.sensors) {
                format_sensors(&sens, l2, sizeof l2);
                format_cores(&sens, l3, sizeof l3);
            }
            snprintf(txt, sizeof txt, "%s\n", l1);
            if (l2[0]) app(txt, sizeof txt, "%s\n", l2);
            if (l3[0]) app(txt, sizeof txt, "%s\n", l3);
            write_file_atomic("status.txt", txt);

            format_kv(&r, &sens, kv, sizeof kv);
            write_file_atomic("status.kv", kv);
            
            if (cfg.overlay) {
                char orows[PERF_ROWS][PERF_ROW_LEN];
                int on = build_rows(&cfg, &r, &sens, orows);
                overlay_send(&cfg, orows, on);
            }

            if (cfg.notify_sec > 0 && !r.idle && (t - last_notify) >= (uint64_t)cfg.notify_sec * 1000000000ull) {
                char toast[512];
                snprintf(toast, sizeof toast, "%s", l1);
                if (l2[0]) app(toast, sizeof toast, "\n%s", l2);
                notify(toast);
                last_notify = t;
            }
            if (access(stop_path, F_OK) == 0) { unlink(stop_path); break; }
        }
        usleep((useconds_t)cfg.poll_us);
    }

    notify("perfbar stopped");
    if (dce.fd >= 0) close(dce.fd);
    close(lock);
    return 0;
}

#ifdef PERFBAR_EMBED
static void *perfbar_thread(void *arg) {
    (void)arg; (void)perfbar_run(0); return NULL;
}

int perfbar_embedded_start(void) {
    pthread_t th; pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return -1;
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    int rc = pthread_create(&th, &at, perfbar_thread, NULL);
    pthread_attr_destroy(&at);
    return rc;
}
#else
int main(void) {
    return perfbar_run(1);
}
#endif
