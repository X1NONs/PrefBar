/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 * Modifications Copyright (C) 2026 erickdavestech
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5_injection.hpp"
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
#include <unistd.h>

namespace prefbar::ps5 {
namespace {

constexpr const char* kMarker = "/system_tmp/prefbar_shellui.pid";
constexpr const char* kLog = "/data/prefbar/injector.log";
constexpr const char* kShellUiName = "SceShellUI";
pid_t g_last_shellui_pid = -1;

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

bool marker_matches(pid_t pid) {
    FILE* fp = std::fopen(kMarker, "r");
    if (!fp)
        return false;

    int marked_pid = -1;
    const int rc = std::fscanf(fp, "%d", &marked_pid);
    std::fclose(fp);
    return rc == 1 && marked_pid == pid;
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
    static bool renderer_quarantined = false;

    if (renderer_quarantined)
        return false;

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

    if (marker_matches(pid))
        return true;

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
        /* Do not stack another copy if the remote image may already exist. */
        retry_allowed = !injected.elf_loaded;
        retry_delay_ticks = retry_allowed ? 5U : 0U;
        log_line(
            "ShellUI stable bootstrap failed pid=%d retry=%s",
            pid,
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
