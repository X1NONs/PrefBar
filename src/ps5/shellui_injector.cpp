/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 * Modifications Copyright (C) 2026 erickdavestech
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5_injection.hpp"
#include "ps5_platform.hpp"
#include "prefbar/build_info.h"
#include "prefbar/crash_strikes.hpp"
#include "prefbar/shellui_stage.hpp"

extern const std::uint8_t prefbar_shellui_elf[];
extern const std::size_t prefbar_shellui_elf_size;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prefbar::ps5 {
namespace {

constexpr const char* kMarker = "/system_tmp/prefbar_shellui.pid";
constexpr const char* kLog = "/data/prefbar/injector.log";
constexpr const char* kShellUiName = "SceShellUI";
constexpr const char* kStrikeDir = "/data/prefbar";
constexpr off_t kInjectorLogMaxBytes = 64 * 1024;
constexpr const char* kForceRendererPath = "/data/prefbar/force_renderer";
pid_t g_last_shellui_pid = -1;
char g_strike_path[96]{}; /* empty until the SDK version is known */

std::uint64_t now_ms() noexcept {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<std::uint64_t>(tv.tv_sec) * 1000ULL +
        static_cast<std::uint64_t>(tv.tv_usec) / 1000ULL;
}

/*
 * A prior FW 9.60 hardware validation established this Sony kinfo_proc
 * layout. PS5 records differ from the stock FreeBSD structure, so use checked
 * byte offsets instead of casting the data to a host SDK structure.
 */
constexpr long kSysctlSyscall = 202;
constexpr int kCtlKern = 1;
constexpr int kKernProc = 14;
constexpr int kKernProcProc = 8;
constexpr std::size_t kPidOffset = 72;
constexpr std::size_t kTdnameOffset = 447;

enum class ShellUiLookupPath {
    None,
    SysctlThreadName,
};

struct ShellUiLookupResult {
    pid_t pid = -1;
    ShellUiLookupPath path = ShellUiLookupPath::None;
    int size_query_rc = -1;
    int data_query_rc = -1;
    int saved_errno = 0;
    std::size_t bytes = 0;
    unsigned records = 0;
    unsigned malformed_records = 0;
};

bool exact_process_name(
    const char* field,
    std::size_t field_size) noexcept {

    constexpr std::size_t name_size = sizeof("SceShellUI") - 1;
    return field_size > name_size &&
        std::memcmp(field, kShellUiName, name_size) == 0 &&
        field[name_size] == '\0';
}

const char* lookup_path_name(ShellUiLookupPath path) noexcept {
    switch (path) {
    case ShellUiLookupPath::SysctlThreadName:
        return "sysctl_tdname";
    case ShellUiLookupPath::None:
        break;
    }
    return "none";
}

ShellUiLookupResult find_shellui_pid() noexcept {
    ShellUiLookupResult result{};

    int mib[4] = {
        kCtlKern,
        kKernProc,
        kKernProcProc,
        0,
    };

    std::size_t required = 0;
    result.size_query_rc = static_cast<int>(syscall(
        kSysctlSyscall,
        mib,
        4,
        nullptr,
        &required,
        nullptr,
        0));

    if (result.size_query_rc == 0 && required != 0) {
        /* Leave headroom if the process list grows between the two calls. */
        const std::size_t capacity = required + required / 4 + 4096;
        auto* buffer = static_cast<std::uint8_t*>(std::malloc(capacity));

        if (buffer) {
            result.bytes = capacity;
            result.data_query_rc = static_cast<int>(syscall(
                kSysctlSyscall,
                mib,
                4,
                buffer,
                &result.bytes,
                nullptr,
                0));

            if (result.data_query_rc == 0 && result.bytes <= capacity) {
                std::uint8_t* cursor = buffer;
                const std::uint8_t* const end = buffer + result.bytes;

                while (cursor < end) {
                    const std::size_t remaining =
                        static_cast<std::size_t>(end - cursor);
                    if (remaining < sizeof(int)) {
                        ++result.malformed_records;
                        break;
                    }

                    int record_size = 0;
                    std::memcpy(
                        &record_size,
                        cursor,
                        sizeof(record_size));
                    if (record_size <= 0 ||
                        static_cast<std::size_t>(record_size) > remaining) {
                        ++result.malformed_records;
                        break;
                    }

                    ++result.records;
                    const std::size_t usable =
                        static_cast<std::size_t>(record_size);
                    const bool has_proven_fields = usable >=
                        kTdnameOffset + sizeof("SceShellUI");

                    pid_t candidate_pid = -1;
                    if (has_proven_fields) {
                        std::memcpy(
                            &candidate_pid,
                            cursor + kPidOffset,
                            sizeof(candidate_pid));
                    }

                    if (candidate_pid > 0 &&
                        candidate_pid != getpid() &&
                        exact_process_name(
                            reinterpret_cast<const char*>(
                                cursor + kTdnameOffset),
                            usable - kTdnameOffset)) {
                        result.pid = candidate_pid;
                        result.path =
                            ShellUiLookupPath::SysctlThreadName;
                        break;
                    }

                    cursor += record_size;
                }
            } else {
                result.saved_errno = result.data_query_rc == 0 ? EOVERFLOW : errno;
                if (result.data_query_rc == 0)
                    result.data_query_rc = -1;
            }

            std::free(buffer);
        } else {
            result.saved_errno = ENOMEM;
        }
    } else {
        result.saved_errno = errno;
    }

    return result;
}

void log_line(const char* fmt, ...) {
    struct stat current{};
    if (stat(kLog, &current) == 0 &&
        current.st_size > kInjectorLogMaxBytes) {
        char backup[128]{};
        std::snprintf(backup, sizeof(backup), "%s.1", kLog);
        (void)unlink(backup);
        (void)rename(kLog, backup);
    }

    FILE* fp = std::fopen(kLog, "a");
    if (!fp)
        return;

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fputc('\n', fp);
    std::fclose(fp);
}

enum class RendererMarkerState {
    Missing,
    CurrentBuild,
    StaleBuild,
};

RendererMarkerState renderer_marker_state(pid_t pid) {
    FILE* fp = std::fopen(kMarker, "r");
    if (!fp)
        return RendererMarkerState::Missing;

    int marked_pid = -1;
    char marked_version[32]{};
    const int fields = std::fscanf(fp, "%d %31s", &marked_pid, marked_version);
    std::fclose(fp);

    if (fields < 1 || marked_pid != pid)
        return RendererMarkerState::Missing;
    if (fields == 2 && std::strcmp(marked_version, PREFBAR_VERSION) == 0)
        return RendererMarkerState::CurrentBuild;
    return RendererMarkerState::StaleBuild;
}

bool marker_matches(pid_t pid) {
    return renderer_marker_state(pid) == RendererMarkerState::CurrentBuild;
}

const char* last_stage_for(pid_t pid, char (&stage)[48]) {
    std::memcpy(stage, "unavailable", sizeof("unavailable"));
    FILE* fp = std::fopen(kShellUiStagePath, "r");
    if (!fp)
        return stage;

    int recorded_pid = -1;
    char recorded_stage[48]{};
    const int count = std::fscanf(
        fp, "%d %47s", &recorded_pid, recorded_stage);
    std::fclose(fp);
    if (count == 2 && recorded_pid == pid)
        std::memcpy(stage, recorded_stage, sizeof(recorded_stage));
    return stage;
}

} // namespace

bool ensure_shellui_renderer() {
    static pid_t attempted_pid = -1;
    static bool attempted_result = false;
    static bool retry_allowed = true;
    static unsigned retry_delay_ticks = 0;
    static unsigned discovery_failures = 0;
    static unsigned injection_attempts = 0;
    static pid_t stale_marker_pid = -1;
    static bool renderer_quarantined = false;
    static bool firmware_gate_checked = false;
    static bool firmware_supported = false;

    if (renderer_quarantined)
        return false;

    /* Reject unknown or unsupported firmware before touching ShellUI. */
    if (!firmware_gate_checked) {
        firmware_gate_checked = true;
        const std::uint32_t sdk = firmware_sdk_version();
        firmware_supported = sdk != 0 &&
            (shellui_hook_backend(sdk, false) != ShellUiHookBackend::Unsupported ||
             shellui_main_thread_guard_backend(sdk) != ShellUiHookBackend::Unsupported);
        if (!firmware_supported) {
            log_line(
                "ShellUI injection blocked: firmware SDK 0x%08x has no supported renderer backend",
                sdk);
        } else {
            log_line("ShellUI firmware preflight passed sdk=0x%08x", sdk);
        }
    }
    if (!firmware_supported)
        return false;

    /*
     * Crash-loop protection: if ShellUI already died while PrefBar was starting
     * on this firmware in earlier launches, do not inject again until the user
     * deletes the strike file or creates the force-renderer file.
     */
    static bool strike_gate_checked = false;
    if (!strike_gate_checked) {
        strike_gate_checked = true;
        const std::uint32_t sdk = firmware_sdk_version();
        if (sdk != 0) {
            renderer_crash_strike_path(
                sdk, kStrikeDir, g_strike_path, sizeof(g_strike_path));
            const bool force = access(kForceRendererPath, F_OK) == 0;
            if (renderer_crash_blocked(g_strike_path, force)) {
                log_line(
                    "ShellUI renderer disabled: firmware sdk=0x%08x crashed "
                    "ShellUI during %u earlier launches. Delete %s to retry, "
                    "or create %s to ignore this protection.",
                    sdk,
                    renderer_crash_strikes(g_strike_path),
                    g_strike_path,
                    kForceRendererPath);
                renderer_quarantined = true;
                return false;
            }
        }
    }

    const ShellUiLookupResult lookup = find_shellui_pid();
    if (lookup.pid <= 0) {
        ++discovery_failures;
        if (discovery_failures == 1 || discovery_failures % 30 == 0) {
            log_line(
                "ShellUI lookup failed sysctl=%d/%d errno=%d "
                "bytes=%zu records=%u malformed=%u",
                lookup.size_query_rc,
                lookup.data_query_rc,
                lookup.saved_errno,
                lookup.bytes,
                lookup.records,
                lookup.malformed_records);
        }
        return false;
    }

    discovery_failures = 0;
    const pid_t pid = lookup.pid;
    g_last_shellui_pid = pid;

    const RendererMarkerState marker = renderer_marker_state(pid);
    if (marker == RendererMarkerState::CurrentBuild)
        return true;
    if (marker == RendererMarkerState::StaleBuild) {
        if (stale_marker_pid != pid) {
            log_line(
                "stale ShellUI renderer marker pid=%d; restart ShellUI before loading this PrefBar build",
                pid);
            stale_marker_pid = pid;
        }
        if (attempted_pid != pid) {
            attempted_pid = pid;
            attempted_result = false;
        }
        retry_allowed = false;
        retry_delay_ticks = 0;
        return false;
    }

    if (attempted_pid != pid) {
        log_line(
            "ShellUI lookup pid=%d via=%s bytes=%zu records=%u malformed=%u",
            pid,
            lookup_path_name(lookup.path),
            lookup.bytes,
            lookup.records,
            lookup.malformed_records);
        attempted_pid = pid;
        attempted_result = false;
        retry_allowed = true;
        retry_delay_ticks = 0;
    } else if (attempted_result) {
        return true;
    } else if (!retry_allowed) {
        return false;
    } else if (retry_delay_ticks != 0) {
        --retry_delay_ticks;
        return false;
    }

    constexpr unsigned kMaxInjectionAttempts = 3;
    if (injection_attempts >= kMaxInjectionAttempts) {
        renderer_quarantined = true;
        retry_allowed = false;
        log_line(
            "ShellUI injection quarantined after %u attempts for this payload lifetime",
            injection_attempts);
        return false;
    }

    ++injection_attempts;
    attempted_result = false;
    clear_shellui_hook_protocol_files();
    (void)unlink(kShellUiStagePath);
    log_line(
        "ShellUI inject start pid=%d payload_size=%zu",
        pid,
        prefbar_shellui_elf_size);

    const std::uint64_t inject_started_ms = now_ms();
    const StableInjectionResult injected = inject_shellui_stable(
        pid,
        prefbar_shellui_elf,
        prefbar_shellui_elf_size);

    log_line(
        "ShellUI bootstrap auth=%d/%d attach=%d load=%d args=%d "
        "target_stack=%d thread=%d rc=%d detach=%d trace=%d/%d "
        "imports=%u/%u first=%s elapsed_ms=%llu",
        injected.auth_changed ? 1 : 0,
        injected.auth_restored ? 1 : 0,
        injected.attached ? 1 : 0,
        injected.elf_loaded ? 1 : 0,
        injected.payload_args_ready ? 1 : 0,
        injected.target_stack_preserved ? 1 : 0,
        injected.bootstrap_started ? 1 : 0,
        injected.pthread_create_rc,
        injected.detached ? 1 : 0,
        injected.trace_continue_seen ? 1 : 0,
        injected.trace_stop_seen ? 1 : 0,
        injected.imports_resolved,
        injected.imports_unresolved,
        injected.first_unresolved && *injected.first_unresolved
            ? injected.first_unresolved
            : "none",
        static_cast<unsigned long long>(now_ms() - inject_started_ms));

    if (!injected.pthread_create_ok || !injected.auth_restored) {
        /* Never retry after a remote image may exist or auth restoration failed. */
        retry_allowed = injected.auth_restored &&
            !injected.elf_loaded &&
            injection_attempts < kMaxInjectionAttempts;
        retry_delay_ticks = retry_allowed
            ? (5U << (injection_attempts - 1U))
            : 0U;
        if (!injected.auth_restored ||
            injection_attempts >= kMaxInjectionAttempts)
            renderer_quarantined = true;
        log_line(
            "ShellUI stable bootstrap failed pid=%d attempts=%u/%u retry=%s",
            pid,
            injection_attempts,
            kMaxInjectionAttempts,
            retry_allowed ? "yes" : "no");
        return false;
    }

    /* Wait for the renderer's own marker and service hook requests meanwhile. */
    for (int i = 0; i < 3500; ++i) {
        if (marker_matches(pid)) {
            attempted_result = true;
            log_line(
                "ShellUI renderer online pid=%d total_ms=%llu",
                pid,
                static_cast<unsigned long long>(now_ms() - inject_started_ms));
            renderer_crash_strikes_clear(g_strike_path);
            return true;
        }

        if (i != 0 && i % 50 == 0) {
            const pid_t observed_pid = find_shellui_pid().pid;
            if (observed_pid > 0 && observed_pid != pid) {
                char stage[48]{};
                log_line(
                    "ShellUI changed before marker old_pid=%d new_pid=%d "
                    "last_stage=%s quarantine=1",
                    pid,
                    observed_pid,
                    last_stage_for(pid, stage));
                renderer_quarantined = true;
                retry_allowed = false;
                const unsigned strikes = renderer_crash_strike_add(g_strike_path);
                if (strikes != 0)
                    log_line(
                        "ShellUI crash strike %u/%u recorded in %s",
                        strikes,
                        kRendererCrashStrikeLimit,
                        g_strike_path);
                return false;
            }
        }

        ShellUiHookPatchReport hook_report{};
        const ShellUiHookPollResult hook_result =
            poll_and_apply_shellui_hook(pid, hook_report);
        if (hook_result != ShellUiHookPollResult::NoRequest) {
            log_line(
                "ShellUI hook request pid=%d sdk=0x%08x mode=%s backend=%s "
                "phase=%s method=0x%llx displaced=%u status=%d "
                "read_rc=%d read_raw_rc=%d read_attempted=%d "
                "read_errno=%d read_reported_len=%u read_requested=%u "
                "probe_mdbg_attempted=%d probe_mdbg_rc=%d probe_mdbg_errno=%d "
                "write_attempted=%d write_rc=%d "
                "expected=%d verified=%d "
                "restored=%d detached=%d auth_restored=%d",
                pid,
                hook_report.sdk_version,
                hook_report.main_thread_guard
                    ? "thread-guard"
                    : (hook_report.eta_chain ? "eta-chain" : "native"),
                hook_report.backend,
                hook_report.probe
                    ? "probe"
                    : (hook_report.main_thread_guard ? "guard" : "patch"),
                static_cast<unsigned long long>(hook_report.method_address),
                hook_report.displaced_size,
                static_cast<int>(hook_report.status),
                hook_report.read_rc,
                hook_report.read_raw_rc,
                hook_report.read_attempted ? 1 : 0,
                hook_report.read_errno,
                hook_report.read_reported_len,
                hook_report.read_requested,
                hook_report.probe_mdbg_attempted ? 1 : 0,
                hook_report.probe_mdbg_rc,
                hook_report.probe_mdbg_errno,
                hook_report.write_attempted ? 1 : 0,
                hook_report.write_rc,
                hook_report.expected_matched ? 1 : 0,
                hook_report.verified ? 1 : 0,
                hook_report.restored ? 1 : 0,
                hook_report.detached ? 1 : 0,
                hook_report.auth_restored ? 1 : 0);
        }
        usleep(20000);
    }

    char stage[48]{};
    log_line(
        "ShellUI marker timeout pid=%d last_stage=%s quarantine=1",
        pid,
        last_stage_for(pid, stage));
    renderer_quarantined = true;
    retry_allowed = false;
    return false;
}

pid_t observe_shellui_pid() noexcept {
    const ShellUiLookupResult lookup = find_shellui_pid();
    g_last_shellui_pid = lookup.pid;
    return lookup.pid;
}

pid_t shellui_renderer_pid() noexcept {
    return g_last_shellui_pid;
}

} // namespace prefbar::ps5
