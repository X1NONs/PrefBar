/*
 * PrefBar PS5 platform interfaces.
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#include "prefbar/game_session.hpp"

namespace prefbar::ps5 {

/* Returns the SDK version reported by the running PS5, or zero if unavailable. */
[[nodiscard]] std::uint32_t firmware_sdk_version() noexcept;

struct ProcessLookupResult {
    /* A unique, verified non-system eboot.bin candidate bootstraps a session. */
    pid_t pid = -1;
    GameApplicationIdentity launch_identity{};
    char launch_command[24]{};
    bool launch_ambiguous = false;

    /* Every process matching the active application's app identity. */
    pid_t tracked_pid = -1;
    unsigned tracked_process_count = 0;
    GameApplicationIdentity tracked_identity{};
    char tracked_command[24]{};

    int size_query_rc = -1;
    int data_query_rc = -1;
    int saved_errno = 0;
    std::size_t bytes = 0;
    unsigned records = 0;
    unsigned malformed_records = 0;
};

/*
 * With no tracked identity, find a unique non-system eboot.bin candidate.
 * During an active session, enumerate process records and match each
 * sceKernelGetAppInfo identity against the active application.
 */
[[nodiscard]] ProcessLookupResult find_game_process(
    const GameApplicationIdentity* tracked_identity = nullptr,
    pid_t tracked_pid = -1) noexcept;

} // namespace prefbar::ps5
