/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 * Modifications Copyright (C) 2026 erickdavestech
 *
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
constexpr std::size_t kTdnameOffset = 447U;
constexpr char kGameName[] = "eboot.bin";

extern "C" {
struct app_info_t {
    std::uint32_t app_id;
    std::uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
};
int sceKernelGetAppInfo(pid_t pid, app_info_t* info);
}

bool record_name_equals(
    const std::uint8_t* record,
    std::size_t record_size) noexcept {

    if (!record || record_size <= kTdnameOffset)
        return false;

    constexpr std::size_t wanted_size = sizeof(kGameName);
    const std::size_t available = record_size - kTdnameOffset;
    return available >= wanted_size &&
        std::memcmp(record + kTdnameOffset, kGameName, wanted_size) == 0;
}

} // namespace

ProcessLookupResult find_game_process() noexcept {
    ProcessLookupResult result{};
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

    if (result.size_query_rc != 0 || required == 0) {
        result.saved_errno = errno;
        return result;
    }

    /* Allow the process list to grow between the two sysctl calls. */
    const std::size_t capacity = required + required / 4U + 4096U;
    auto* buffer = static_cast<std::uint8_t*>(std::malloc(capacity));
    if (!buffer) {
        result.saved_errno = ENOMEM;
        return result;
    }

    result.bytes = capacity;
    result.data_query_rc = static_cast<int>(syscall(
        kSysctlSyscall,
        mib,
        4,
        buffer,
        &result.bytes,
        nullptr,
        0));

    if (result.data_query_rc != 0 || result.bytes > capacity) {
        result.saved_errno = errno;
        std::free(buffer);
        return result;
    }

    const pid_t self = getpid();
    const std::uint8_t* cursor = buffer;
    const std::uint8_t* const end = buffer + result.bytes;

    while (cursor < end) {
        const std::size_t remaining =
            static_cast<std::size_t>(end - cursor);
        if (remaining < sizeof(int)) {
            ++result.malformed_records;
            break;
        }

        int record_size_signed = 0;
        std::memcpy(
            &record_size_signed,
            cursor,
            sizeof(record_size_signed));

        if (record_size_signed <= 0 ||
            static_cast<std::size_t>(record_size_signed) > remaining) {
            ++result.malformed_records;
            break;
        }

        ++result.records;
        const std::size_t record_size =
            static_cast<std::size_t>(record_size_signed);

        if (record_size >= kPidOffset + sizeof(pid_t) &&
            record_name_equals(cursor, record_size)) {
            pid_t candidate = -1;
            std::memcpy(&candidate, cursor + kPidOffset, sizeof(candidate));
            if (candidate > 0 && candidate != self) {
                app_info_t info{};
                if (sceKernelGetAppInfo(candidate, &info) == 0) {
                    if (std::strncmp(info.title_id, "NPXS", 4) != 0 && info.title_id[0] != '\0') {
                        result.pid = candidate;
                        std::snprintf(result.title_id, sizeof(result.title_id), "%s", info.title_id);
                    }
                } else {
                    result.pid = candidate;
                }
            }
        }

        cursor += record_size;
    }

    std::free(buffer);
    return result;
}

} // namespace prefbar::ps5
