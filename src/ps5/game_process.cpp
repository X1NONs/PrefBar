/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 * Modifications Copyright (C) 2026 erickdavestech
 * Modifications Copyright (C) 2026 X1NONs
 *
 * Process-record layout and application-ID matching follow publicly used
 * PS5 KERN_PROC / sceKernelGetAppInfo patterns (see CREDITS.md).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5_platform.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace prefbar::ps5 {
namespace {

constexpr long kSysctlSyscall = 202;
constexpr int kCtlKern = 1;
constexpr int kKernProc = 14;
constexpr int kKernProcProc = 8;
constexpr std::size_t kPidOffset = 72U;
/* Offset 447 is kinfo_proc::ki_comm (process command), not a thread name. */
constexpr std::size_t kCommOffset = 447U;
constexpr std::size_t kCommLength = 20U;
constexpr char kGameName[] = "eboot.bin";
constexpr std::size_t kProcessSnapshotLimit = 16U * 1024U * 1024U;
constexpr unsigned kSnapshotAttempts = 3;

extern "C" {
struct app_info_t {
    std::uint32_t app_id;
    std::uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
};
int sceKernelGetAppInfo(pid_t pid, app_info_t* info);
}

[[nodiscard]] bool record_name_equals(
    const std::uint8_t* record,
    std::size_t record_size) noexcept {
    constexpr std::size_t wanted_size = sizeof(kGameName);
    if (!record || record_size < kCommOffset + wanted_size)
        return false;
    return std::memcmp(record + kCommOffset, kGameName, wanted_size) == 0;
}

void copy_process_command(
    char (&out)[24],
    const std::uint8_t* record,
    std::size_t record_size) noexcept {
    std::memset(out, 0, sizeof(out));
    if (!record || record_size <= kCommOffset)
        return;
    std::size_t available = record_size - kCommOffset;
    if (available > kCommLength)
        available = kCommLength;
    std::size_t length = 0;
    while (length < available && length < sizeof(out) - 1 &&
           record[kCommOffset + length] != 0)
        ++length;
    std::memcpy(out, record + kCommOffset, length);
    out[length] = '\0';
}

[[nodiscard]] bool is_system_title(const app_info_t& info) noexcept {
    return std::strncmp(info.title_id, "NPXS", 4) == 0;
}

[[nodiscard]] GameApplicationIdentity application_identity(
    const app_info_t& info) noexcept {
    GameApplicationIdentity identity{};
    if (is_system_title(info))
        return identity;

    identity.app_id = info.app_id;
    if (info.title_id[0] != '\0') {
        std::memcpy(identity.title_id, info.title_id, sizeof(info.title_id));
        identity.title_id[sizeof(info.title_id)] = '\0';
    }
    return identity;
}

[[nodiscard]] bool same_candidate_identity(
    const GameApplicationIdentity& a,
    pid_t a_pid,
    const GameApplicationIdentity& b,
    pid_t b_pid) noexcept {
    if (a.valid() && b.valid())
        return same_game_application(a, b);
    return a_pid == b_pid;
}

} // namespace

ProcessLookupResult find_game_process(
    const GameApplicationIdentity* tracked_identity,
    pid_t tracked_pid) noexcept {
    ProcessLookupResult result{};
    int mib[4] = {kCtlKern, kKernProc, kKernProcProc, 0};

    std::uint8_t* buffer = nullptr;
    std::size_t used = 0;
    for (unsigned attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
        std::size_t required = 0;
        result.size_query_rc = static_cast<int>(syscall(
            kSysctlSyscall, mib, 4, nullptr, &required, nullptr, 0));
        if (result.size_query_rc != 0) {
            result.saved_errno = errno;
            return result;
        }
        /* A valid KERN_PROC/PROC snapshot cannot be empty: at minimum the
           system and this controller process must exist. Treat zero bytes as
           an incomplete observation, never as proof that the game exited. */
        if (required == 0) {
            result.data_query_rc = -1;
            result.saved_errno = EPROTO;
            return result;
        }
        if (required > kProcessSnapshotLimit) {
            result.data_query_rc = -1;
            result.saved_errno = E2BIG;
            return result;
        }

        const std::size_t extra = required / 4U + 4096U;
        const std::size_t capacity =
            extra > kProcessSnapshotLimit - required
                ? kProcessSnapshotLimit
                : required + extra;
        buffer = static_cast<std::uint8_t*>(std::malloc(capacity));
        if (!buffer) {
            result.data_query_rc = -1;
            result.saved_errno = ENOMEM;
            return result;
        }

        used = capacity;
        result.data_query_rc = static_cast<int>(syscall(
            kSysctlSyscall, mib, 4, buffer, &used, nullptr, 0));
        if (result.data_query_rc == 0 && used > 0 && used <= capacity)
            break;

        const int saved_errno = result.data_query_rc != 0
            ? errno
            : used == 0 ? EPROTO : EOVERFLOW;
        std::free(buffer);
        buffer = nullptr;
        result.saved_errno = saved_errno;
        if (saved_errno != ENOMEM || attempt + 1U == kSnapshotAttempts) {
            if (result.data_query_rc == 0)
                result.data_query_rc = -1;
            return result;
        }
    }

    if (!buffer) {
        result.data_query_rc = -1;
        if (!result.saved_errno)
            result.saved_errno = ENOMEM;
        return result;
    }

    result.bytes = used;
    result.saved_errno = 0;
    const pid_t self = getpid();
    const bool have_tracked_identity =
        tracked_identity && tracked_identity->valid();
    const std::uint8_t* cursor = buffer;
    const std::uint8_t* const end = buffer + used;
    bool selected_tracked_is_eboot = false;

    while (cursor < end) {
        const std::size_t remaining = static_cast<std::size_t>(end - cursor);
        if (remaining < sizeof(int)) {
            ++result.malformed_records;
            break;
        }

        int record_size_signed = 0;
        std::memcpy(&record_size_signed, cursor, sizeof(record_size_signed));
        if (record_size_signed < 0 ||
            static_cast<std::size_t>(record_size_signed) > remaining ||
            static_cast<std::size_t>(record_size_signed) <
                kPidOffset + sizeof(pid_t)) {
            ++result.malformed_records;
            break;
        }

        ++result.records;
        const std::size_t record_size =
            static_cast<std::size_t>(record_size_signed);
        pid_t candidate_pid = -1;
        std::memcpy(&candidate_pid, cursor + kPidOffset, sizeof(candidate_pid));
        const bool is_eboot = record_name_equals(cursor, record_size);

        if (candidate_pid > 0 && candidate_pid != self &&
            (is_eboot || have_tracked_identity || candidate_pid == tracked_pid)) {
            app_info_t info{};
            const bool app_info_ok =
                sceKernelGetAppInfo(candidate_pid, &info) == 0;
            const GameApplicationIdentity identity = app_info_ok
                ? application_identity(info)
                : GameApplicationIdentity{};

            bool matches_tracked = false;
            if (have_tracked_identity && app_info_ok && identity.valid()) {
                matches_tracked =
                    same_game_application(*tracked_identity, identity);
            } else if (!have_tracked_identity && tracked_pid > 0 &&
                       candidate_pid == tracked_pid && is_eboot) {
                /* Compatibility fallback only when the initial app identity
                   could not be obtained. Never use PID alone over a known app. */
                matches_tracked = true;
            }

            if (matches_tracked) {
                ++result.tracked_process_count;
                if (result.tracked_pid < 0 || candidate_pid == tracked_pid ||
                    (is_eboot && !selected_tracked_is_eboot)) {
                    result.tracked_pid = candidate_pid;
                    result.tracked_identity = identity;
                    copy_process_command(result.tracked_command,
                                         cursor, record_size);
                    selected_tracked_is_eboot = is_eboot;
                }
            }

            /* eboot.bin is only a bootstrap hint. Require verified, non-system
               app metadata before treating it as a game candidate; an API
               race must not turn an unknown/system process into a game. */
            if (is_eboot && app_info_ok && !is_system_title(info) &&
                identity.valid()) {
                const bool is_current_application =
                    have_tracked_identity &&
                    same_game_application(*tracked_identity, identity);
                if (!is_current_application) {
                    if (result.pid < 0) {
                        result.pid = candidate_pid;
                        result.launch_identity = identity;
                        copy_process_command(result.launch_command,
                                             cursor, record_size);
                    } else if (!same_candidate_identity(
                                   result.launch_identity, result.pid,
                                   identity, candidate_pid)) {
                        result.launch_ambiguous = true;
                    }
                }
            }
        }

        cursor += record_size;
    }

    std::free(buffer);

    /* A malformed tail means the snapshot was not complete. Do not let a
       partial list drive an application-exit decision. */
    if (result.malformed_records != 0) {
        result.data_query_rc = -1;
        result.saved_errno = EPROTO;
        result.pid = -1;
        result.launch_identity = {};
        result.launch_command[0] = '\0';
        result.launch_ambiguous = false;
        result.tracked_pid = -1;
        result.tracked_process_count = 0;
        result.tracked_identity = {};
        result.tracked_command[0] = '\0';
        return result;
    }

    if (result.launch_ambiguous) {
        result.pid = -1;
        result.launch_identity = {};
        result.launch_command[0] = '\0';
    }
    result.data_query_rc = 0;
    return result;
}

} // namespace prefbar::ps5
