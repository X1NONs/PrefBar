/*
 * PrefBar PS5 controller
 * Copyright (C) 2026 X1NONs
 * Portions of the ShellUI discovery/injection path derive from the GPL sources
 * listed in CREDITS.md and retain their original file-level attribution.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5_platform.hpp"
#include "ps5_injection.hpp"
#include "web_server.hpp"
#ifndef PREFBAR_NO_WEB
#include "app_launcher.hpp"
#endif

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <pthread.h>
#include <signal.h>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PBDBG_ERR   1
#define PBDBG_WARN  2
#define PBDBG_INFO  3
#define PBDBG_TRACE 4

extern "C" void prefbar_debug_event(
    int level, const char* key, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
extern "C" int prefbar_telemetry_embedded_start(void);

namespace {

constexpr const char* kControllerLog = "/data/prefbar/controller.log";
constexpr char kBootLog[] = "/data/prefbar/boot.log";
constexpr unsigned kStartupRetryIntervalUs = 2'000'000;
#ifndef PREFBAR_NO_WEB
constexpr unsigned kWebRetryIntervalUs = 5'000'000;
#endif
constexpr unsigned kShellUiStableSamples = 10;
constexpr unsigned kGameStableSamples = 3;
constexpr unsigned kGameExitGraceSamples = 10; /* 500 ms at the controller poll rate */
constexpr std::uint64_t kRendererWarningDelayUs = 20'000'000ULL;
constexpr std::uint64_t kRendererWarningRepeatUs = 60'000'000ULL;
constexpr unsigned kMaxRendererWarnings = 3;
constexpr useconds_t kPollIntervalUs = 50'000;

/* Read by the embedded telemetry thread before it touches /dev/dce. */
std::atomic<int> g_game_process_ready{0};
std::atomic<std::uint64_t> g_game_session_generation{0};

std::uint64_t monotonic_us() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ULL +
        static_cast<std::uint64_t>(ts.tv_nsec) / 1'000ULL;
}

#ifndef PREFBAR_NO_WEB
struct StartupNotificationState {
    std::atomic<int> web_rc{-1};
    std::atomic<int> telemetry_rc{-1};
};

/* Static storage: the detached notification worker may outlive startup. */
StartupNotificationState g_startup_notification_state{};
#endif

void write_boot_stage(const char* stage, int web_rc, int telemetry_rc) noexcept {
    struct stat st{};
    if (stat(kBootLog, &st) == 0 && st.st_size > 32 * 1024) {
        char backup[sizeof(kBootLog) + 3]{};
        std::snprintf(backup, sizeof(backup), "%s.1", kBootLog);
        (void)rename(kBootLog, backup);
    }

    FILE* fp = std::fopen(kBootLog, "a");
    if (!fp)
        return;

    std::fprintf(fp,
                 "t=%llu pid=%d stage=%s web_rc=%d telemetry_rc=%d\n",
                 static_cast<unsigned long long>(monotonic_us()),
                 static_cast<int>(getpid()),
                 stage ? stage : "unknown",
                 web_rc, telemetry_rc);
    (void)std::fflush(fp);
    std::fclose(fp);
}

/* Delay startup notifications until PrefBar's attribution toast has had its
 * normal opportunity to appear. */
#ifndef PREFBAR_NO_WEB
void* startup_notification_worker(void* opaque) noexcept {
    (void)usleep(4'000'000);
    const auto* state =
        static_cast<const StartupNotificationState*>(opaque);
    if (!state)
        return nullptr;

    const int web_rc = state->web_rc.load(std::memory_order_acquire);
    const int telemetry_rc =
        state->telemetry_rc.load(std::memory_order_acquire);
    const bool web_running = prefbar::web::is_running();

    if (web_running && telemetry_rc == 0) {
        prefbar_debug_event(
            PBDBG_INFO, "web.ready",
            "Control Center ready; listening on port %u",
            static_cast<unsigned>(prefbar::web::kDefaultPort));
    } else if (web_running) {
        prefbar_debug_event(
            PBDBG_ERR, "startup.degraded",
            "Control Center listening on port %u; telemetry unavailable "
            "(rc=%d). Retrying telemetry startup; renderer injection is paused",
            static_cast<unsigned>(prefbar::web::kDefaultPort),
            telemetry_rc);
    } else if (telemetry_rc == 0) {
        prefbar_debug_event(
            PBDBG_ERR, "web.unavailable",
            "Telemetry started, but Control Center is not listening "
            "(last web rc=%d); the controller will retry",
            web_rc);
    } else {
        prefbar_debug_event(
            PBDBG_ERR, "startup.failed",
            "Control Center and telemetry are not ready (web rc=%d, "
            "telemetry rc=%d); startup retries are active",
            web_rc, telemetry_rc);
    }
    return nullptr;
}
#endif

void write_startup_record(
    pid_t first_shellui,
    const prefbar::ps5::ProcessLookupResult& game,
    int telemetry_rc,
    int web_rc) noexcept {
    FILE* fp = std::fopen(kControllerLog, "w");
    if (!fp)
        return;

    std::fprintf(
        fp,
        "PrefBar V0.3.1 controller\n"
        "Mode=%s\n"
        "Startup web_rc=%d telemetry_rc=%d http_port=%u\n"
        "Game gate=%u consecutive samples; ShellUI gate=%u consecutive samples; "
        "poll=%u ms\n"
        "Renderer injection=deferred-until-game-and-shellui-ready\n"
        "Telemetry=/dev/dce plus optional PS5 sensor APIs\n"
        "Overlay=loopback UDP rows only; port=39028\n"
        "Worker pid=%d ppid=%d first_shellui_pid=%d first_game_pid=%d "
        "title_id=%s sysctl=%d/%d errno=%d bytes=%zu records=%u malformed=%u\n",
#ifdef PREFBAR_NO_WEB
        "single-controller embedded-telemetry shellui-renderer; web disabled",
        web_rc,
        telemetry_rc,
        0U,
#else
        telemetry_rc == 0
            ? "single-controller embedded-telemetry shellui-renderer"
            : web_rc == 0
                ? "diagnostics-only; renderer injection disabled"
                : "startup-failed; telemetry unavailable; renderer disabled",
        web_rc,
        telemetry_rc,
        static_cast<unsigned>(prefbar::web::kDefaultPort),
#endif
        kGameStableSamples,
        kShellUiStableSamples,
        static_cast<unsigned>(kPollIntervalUs / 1000U),
        static_cast<int>(getpid()),
        static_cast<int>(getppid()),
        static_cast<int>(first_shellui),
        static_cast<int>(game.pid),
        game.title_id[0] ? game.title_id : "unknown",
        game.size_query_rc,
        game.data_query_rc,
        game.saved_errno,
        game.bytes,
        game.records,
        game.malformed_records);

    std::fclose(fp);
}

[[noreturn]] void run_controller() noexcept {
    pid_t stable_shellui_pid = -1;
    unsigned stable_shellui_samples = 0;
    pid_t stable_game_pid = -1;
    unsigned stable_game_samples = 0;
    unsigned missing_game_samples = 0;
    bool renderer_online = false;
    bool renderer_was_reported = false;
    unsigned renderer_warnings = 0;
    std::uint64_t next_renderer_warning_us = 0;
    pid_t reported_game_pid = -1;
    pid_t reported_shellui_pid = -1;
#ifndef PREFBAR_NO_WEB
    std::uint64_t next_web_retry_us = 0;
#endif

    prefbar_debug_event(
        PBDBG_INFO, "controller.start",
        "PrefBar controller started; waiting for ShellUI and a game");

    for (;;) {
        const pid_t observed_shellui_pid = prefbar::ps5::observe_shellui_pid();
        if (observed_shellui_pid > 0) {
            if (observed_shellui_pid == stable_shellui_pid) {
                if (stable_shellui_samples < kShellUiStableSamples)
                    ++stable_shellui_samples;
            } else {
                stable_shellui_pid = observed_shellui_pid;
                stable_shellui_samples = 1;
                renderer_online = false;
                renderer_was_reported = false;
                renderer_warnings = 0;
                reported_shellui_pid = -1;
            }
        } else {
            stable_shellui_pid = -1;
            stable_shellui_samples = 0;
            renderer_online = false;
            renderer_was_reported = false;
            renderer_warnings = 0;
        }

        if (stable_shellui_samples >= kShellUiStableSamples &&
            reported_shellui_pid != stable_shellui_pid) {
            if (reported_shellui_pid > 0) {
                prefbar_debug_event(
                    PBDBG_WARN, "shellui.restart",
                    "ShellUI restarted (pid %d -> %d)",
                    static_cast<int>(reported_shellui_pid),
                    static_cast<int>(stable_shellui_pid));
            } else {
                prefbar_debug_event(
                    PBDBG_INFO, "shellui.ready",
                    "ShellUI detected (pid %d)",
                    static_cast<int>(stable_shellui_pid));
            }
            reported_shellui_pid = stable_shellui_pid;
        }

        const prefbar::ps5::ProcessLookupResult game =
            prefbar::ps5::find_game_process();
        bool game_exit_confirmed = false;
        if (game.pid > 0) {
            missing_game_samples = 0;
            if (game.pid == stable_game_pid) {
                if (stable_game_samples < kGameStableSamples)
                    ++stable_game_samples;
            } else {
                stable_game_pid = game.pid;
                stable_game_samples = 1;
                /* Keep the last confirmed PID until this candidate is stable. */
            }
        } else if (game.size_query_rc != 0 || game.data_query_rc != 0) {
            /* An incomplete sysctl scan is unknown, not proof that the game exited.
             * Preserve readiness for the last confirmed game during this scan. */
            if (reported_game_pid > 0) {
                stable_game_pid = reported_game_pid;
                stable_game_samples = kGameStableSamples;
            }
        } else if (reported_game_pid > 0 &&
                   missing_game_samples < kGameExitGraceSamples) {
            ++missing_game_samples;
            stable_game_pid = reported_game_pid;
            stable_game_samples = kGameStableSamples;
        } else {
            stable_game_pid = -1;
            stable_game_samples = 0;
            game_exit_confirmed = reported_game_pid > 0;
        }

        const bool game_ready =
            stable_game_pid > 0 && stable_game_samples >= kGameStableSamples;
        g_game_process_ready.store(game_ready ? 1 : 0, std::memory_order_relaxed);

        const std::uint64_t now = monotonic_us();

        #ifndef PREFBAR_NO_WEB
        if (!prefbar::web::is_running() && now >= next_web_retry_us) {
            next_web_retry_us = now + kWebRetryIntervalUs;
            const int web_rc = prefbar::web::start();
            g_startup_notification_state.web_rc.store(
                web_rc, std::memory_order_release);
            write_boot_stage("web_watchdog_retry", web_rc, 0);
            if (web_rc == 0) {
                prefbar::launcher::request_install();
                prefbar_debug_event(
                    PBDBG_INFO, "web.recovered",
                    "Control Center listener recovered on port %u",
                    static_cast<unsigned>(prefbar::web::kDefaultPort));
            } else {
                prefbar_debug_event(
                    PBDBG_WARN, "web.retry",
                    "Control Center listener retry failed (rc=%d)", web_rc);
            }
        }
        #endif

        if (game_ready && reported_game_pid != stable_game_pid) {
            reported_game_pid = stable_game_pid;
            g_game_session_generation.fetch_add(1, std::memory_order_relaxed);
            renderer_warnings = 0;
            next_renderer_warning_us = now + kRendererWarningDelayUs;
            prefbar_debug_event(
                PBDBG_INFO, "game.detected",
                "game detected (pid %d, title %s)",
                static_cast<int>(stable_game_pid),
                game.title_id[0] ? game.title_id : "unknown");
        } else if (game_exit_confirmed && reported_game_pid > 0) {
            prefbar_debug_event(
                PBDBG_INFO, "game.exited",
                "game process no longer present (pid %d)",
                static_cast<int>(reported_game_pid));
            reported_game_pid = -1;
            missing_game_samples = 0;
        }

        const bool shellui_ready =
            stable_shellui_pid > 0 &&
            stable_shellui_samples >= kShellUiStableSamples;

        /* Never install the renderer on the home screen. */
        if (!renderer_online && game_ready && shellui_ready) {
            renderer_online = prefbar::ps5::ensure_shellui_renderer();
        }

        if (renderer_online && !renderer_was_reported) {
            renderer_was_reported = true;
            prefbar_debug_event(
                PBDBG_INFO, "renderer.online",
                "PrefBar renderer online in ShellUI (pid %d)",
                static_cast<int>(stable_shellui_pid));
        } else if (!renderer_online && renderer_was_reported) {
            renderer_was_reported = false;
            renderer_warnings = 0;
            next_renderer_warning_us = now + kRendererWarningDelayUs;
            prefbar_debug_event(
                PBDBG_WARN, "renderer.lost",
                "renderer offline after ShellUI change; waiting to reinject");
        }

        if (!renderer_online && game_ready && shellui_ready &&
            renderer_warnings < kMaxRendererWarnings &&
            now >= next_renderer_warning_us) {
            ++renderer_warnings;
            next_renderer_warning_us = now + kRendererWarningRepeatUs;
            prefbar_debug_event(
                PBDBG_WARN, "renderer.slow",
                "renderer still not online (ShellUI samples %u/%u)",
                stable_shellui_samples, kShellUiStableSamples);
        }

        usleep(kPollIntervalUs);
    }
}

} // namespace

extern "C" int prefbar_game_process_ready(void) noexcept {
    return g_game_process_ready.load(std::memory_order_relaxed);
}

extern "C" std::uint64_t prefbar_game_session_generation(void) noexcept {
    return g_game_session_generation.load(std::memory_order_relaxed);
}

/* Used by the HTTP backend to record exact startup checkpoints without
 * depending on the telemetry/debug-notification subsystem. */
extern "C" void prefbar_boot_stage(
    const char* stage, int web_rc, int telemetry_rc) noexcept {
    write_boot_stage(stage, web_rc, telemetry_rc);
}

extern "C" int main() {
    const int mkdir_rc = mkdir("/data/prefbar", 0777);
    const int mkdir_errno = errno;
    write_boot_stage("entry", mkdir_rc, mkdir_errno);

    errno = 0;
    const auto previous_sigpipe = signal(SIGPIPE, SIG_IGN);
    const int signal_errno = errno;
    write_boot_stage(
        "after_sigpipe", previous_sigpipe == SIG_ERR ? -1 : 0,
        previous_sigpipe == SIG_ERR ? signal_errno : 0);

    errno = 0;
    write_boot_stage("before_thread_name", -1, -1);
#ifdef SYS_thr_set_name
    const long process_name_rc = syscall(SYS_thr_set_name, -1L, "PrefBar");
    const int process_name_errno = errno;
#else
    const long process_name_rc = -1;
    const int process_name_errno = ENOSYS;
#endif
    write_boot_stage(
        "after_thread_name", static_cast<int>(process_name_rc),
        process_name_errno);

    int web_rc = -1;
    #ifndef PREFBAR_NO_WEB
    /* Start the Control Center before telemetry so it can report failures. */
    write_boot_stage("before_web_start", -1, -1);
    web_rc = prefbar::web::start();
    write_boot_stage("after_web_start", web_rc, errno);
    if (web_rc == 0) {
        prefbar::launcher::request_install();
        prefbar_debug_event(
            PBDBG_INFO, "web.start",
            "Control Center HTTP server listening on port %u",
            static_cast<unsigned>(prefbar::web::kDefaultPort));
    } else {
        prefbar_debug_event(
            PBDBG_ERR, "web.start",
            "Control Center HTTP server could not start (rc=%d); retrying",
            web_rc);
    }

    #endif

    int telemetry_rc = prefbar_telemetry_embedded_start();
    write_boot_stage("telemetry_start", web_rc, telemetry_rc);

    prefbar_debug_event(
        process_name_rc == 0 ? PBDBG_INFO : PBDBG_WARN,
        "process.name",
        "main thread name request rc=%ld errno=%d",
        process_name_rc, process_name_errno);

    #ifndef PREFBAR_NO_WEB
    g_startup_notification_state.web_rc.store(web_rc, std::memory_order_release);
    g_startup_notification_state.telemetry_rc.store(
        telemetry_rc, std::memory_order_release);

    pthread_t notice_thread{};
    const int notice_rc = pthread_create(
        &notice_thread, nullptr, startup_notification_worker,
        &g_startup_notification_state);
    if (notice_rc == 0) {
        (void)pthread_detach(notice_thread);
    } else {
        prefbar_debug_event(
            PBDBG_WARN, "startup.notice.thread",
            "could not schedule startup notification (rc=%d)", notice_rc);
    }

    #endif

    if (telemetry_rc != 0) {
        prefbar_debug_event(
            PBDBG_ERR, "telemetry.thread",
            "telemetry thread startup failed (rc=%d); retrying without "
            "starting a second renderer controller",
            telemetry_rc);
    }

    /* A failed lock or transient pthread resource error is not permanent.
     * Keep diagnostics reachable and retry; injection begins only after this
     * process owns the telemetry singleton lock and its sampler thread starts. */
    while (telemetry_rc != 0) {
        (void)usleep(kStartupRetryIntervalUs);

#ifndef PREFBAR_NO_WEB
        if (!prefbar::web::is_running()) {
            web_rc = prefbar::web::start();
            g_startup_notification_state.web_rc.store(
                web_rc, std::memory_order_release);
            write_boot_stage("web_start_retry", web_rc, telemetry_rc);
        } else {
            web_rc = 0;
        }
        if (web_rc == 0)
            prefbar::launcher::request_install();
#endif

        telemetry_rc = prefbar_telemetry_embedded_start();
#ifndef PREFBAR_NO_WEB
        g_startup_notification_state.telemetry_rc.store(
            telemetry_rc, std::memory_order_release);
#endif
        write_boot_stage("telemetry_retry", web_rc, telemetry_rc);
    }

    const pid_t shellui_pid = prefbar::ps5::observe_shellui_pid();
    const prefbar::ps5::ProcessLookupResult game =
        prefbar::ps5::find_game_process();
    write_startup_record(shellui_pid, game, telemetry_rc, web_rc);
    write_boot_stage("runtime_start", web_rc, telemetry_rc);

    prefbar_debug_event(
        PBDBG_INFO, "telemetry.retry.success",
        "Telemetry worker started; renderer controller is now active");

    run_controller();
}
