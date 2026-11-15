/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

/*
 * Persistent "ShellUI died while PrefBar was starting" counter, kept per
 * firmware (SDK version) in a small file. After kRendererCrashStrikeLimit
 * strikes the controller stops injecting the renderer on later launches, so a
 * firmware that crashes ShellUI is not black-screened again on every boot or
 * autoload. A successful renderer start clears the counter. Users can retry by
 * deleting the strike file or creating the force-renderer file.
 */
namespace prefbar {

inline constexpr unsigned kRendererCrashStrikeLimit = 2;

inline void renderer_crash_strike_path(
    std::uint32_t sdk, const char* dir, char* out, std::size_t cap) noexcept {
    std::snprintf(out, cap, "%s/shellui_crash_%08x", dir, sdk);
}

inline unsigned renderer_crash_strikes(const char* path) noexcept {
    if (!path || !*path)
        return 0;
    std::FILE* fp = std::fopen(path, "r");
    if (!fp)
        return 0;
    unsigned count = 0;
    if (std::fscanf(fp, "%u", &count) != 1)
        count = 0;
    std::fclose(fp);
    return count > 1000U ? 1000U : count;
}

inline unsigned renderer_crash_strike_add(const char* path) noexcept {
    if (!path || !*path)
        return 0;
    const unsigned count = renderer_crash_strikes(path) + 1U;
    if (std::FILE* fp = std::fopen(path, "w")) {
        std::fprintf(fp, "%u\n", count);
        std::fclose(fp);
    }
    return count;
}

inline void renderer_crash_strikes_clear(const char* path) noexcept {
    if (path && *path)
        (void)unlink(path);
}

[[nodiscard]] inline bool renderer_crash_blocked(
    const char* path, bool force) noexcept {
    return !force && renderer_crash_strikes(path) >= kRendererCrashStrikeLimit;
}

} // namespace prefbar
