/*
 * PrefBar PS5 controller
 * Portions of the ShellUI discovery/injection path derive from the GPL sources
 * listed in CREDITS.md and retain their original file-level attribution.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5_platform.hpp"
#include "ps5_injection.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
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
constexpr unsigned kShellUiStableSamples = 10;
constexpr unsigned kGameStableSamples = 3;
constexpr std::uint64_t kRendererWarningDelayUs = 20'000'000ULL;
constexpr std::uint64_t kRendererWarningRepeatUs = 60'000'000ULL;
constexpr unsigned kMaxRendererWarnings = 3;
constexpr useconds_t kPollIntervalUs = 50'000;

/* Read by the embedded telemetry thread before it touches /dev/dce. */
std::atomic<int> g_game_process_ready{0};

std::uint64_t monotonic_us() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ULL +
        static_cast<std::uint64_t>(ts.tv_nsec) / 1'000ULL;
}

void write_startup_record(pid_t first_shellui,
                          const prefbar::ps5::ProcessLookupResult& game) noexcept {
    FILE* fp = std::fopen(kControllerLog, "w");
    if (!fp)
        return;

    std::fprintf(
        fp,
        "PrefBar V0.2.0 controller\n"
        "Mode=single-controller embedded-telemetry shellui-renderer\n"
        "Game gate=%u consecutive samples; ShellUI gate=%u consecutive samples; "
        "poll=%u ms\n"
        "Renderer injection=deferred-until-game-and-shellui-ready\n"
        "Telemetry=/dev/dce plus optional PS5 sensor APIs\n"
        "Overlay=loopback UDP rows only; port=39028\n"
        "Worker pid=%d ppid=%d first_shellui_pid=%d first_game_pid=%d "
        "title_id=%s sysctl=%d/%d errno=%d bytes=%zu records=%u malformed=%u\n",
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
    bool renderer_online = false;
    bool renderer_was_reported = false;
    unsigned renderer_warnings = 0;
    std::uint64_t next_renderer_warning_us = 0;
    pid_t reported_game_pid = -1;
    pid_t reported_shellui_pid = -1;

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
        if (game.pid > 0) {
            if (game.pid == stable_game_pid) {
                if (stable_game_samples < kGameStableSamples)
                    ++stable_game_samples;
            } else {
                stable_game_pid = game.pid;
                stable_game_samples = 1;
                reported_game_pid = -1;
            }
        } else {
            stable_game_pid = -1;
            stable_game_samples = 0;
        }

        const bool game_ready =
            stable_game_pid > 0 && stable_game_samples >= kGameStableSamples;
        g_game_process_ready.store(game_ready ? 1 : 0, std::memory_order_relaxed);

        const std::uint64_t now = monotonic_us();
        if (game_ready && reported_game_pid != stable_game_pid) {
            reported_game_pid = stable_game_pid;
            renderer_warnings = 0;
            next_renderer_warning_us = now + kRendererWarningDelayUs;
            prefbar_debug_event(
                PBDBG_INFO, "game.detected",
                "game detected (pid %d, title %s)",
                static_cast<int>(stable_game_pid),
                game.title_id[0] ? game.title_id : "unknown");
        } else if (!game_ready && reported_game_pid > 0) {
            prefbar_debug_event(
                PBDBG_INFO, "game.exited",
                "game process no longer present (pid %d)",
                static_cast<int>(reported_game_pid));
            reported_game_pid = -1;
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

extern "C" int main() {
    (void)mkdir("/data/prefbar", 0777);
    errno = 0;
#ifdef SYS_thr_set_name
    const long process_name_rc = syscall(SYS_thr_set_name, -1L, "PrefBar");
    const int process_name_errno = errno;
#else
    const long process_name_rc = -1;
    const int process_name_errno = ENOSYS;
#endif
    const int telemetry_rc = prefbar_telemetry_embedded_start();
    prefbar_debug_event(
        process_name_rc == 0 ? PBDBG_INFO : PBDBG_WARN,
        "process.name",
        "main thread name request rc=%ld errno=%d",
        process_name_rc, process_name_errno);
    if (telemetry_rc != 0) {
        prefbar_debug_event(
            PBDBG_ERR, "telemetry.thread",
            "telemetry thread failed to start (rc=%d)", telemetry_rc);
    }
    const pid_t shellui_pid = prefbar::ps5::observe_shellui_pid();
    const prefbar::ps5::ProcessLookupResult game =
        prefbar::ps5::find_game_process();
    write_startup_record(shellui_pid, game);
    run_controller();
}
