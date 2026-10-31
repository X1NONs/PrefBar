/* PrefBar PS5 platform interfaces. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace prefbar::ps5 {

/* Returns the SDK version reported by the running PS5, or zero if unavailable. */
[[nodiscard]] std::uint32_t firmware_sdk_version() noexcept;

struct ProcessLookupResult {
    pid_t pid = -1;
    int size_query_rc = -1;
    int data_query_rc = -1;
    int saved_errno = 0;
    std::size_t bytes = 0;
    unsigned records = 0;
    unsigned malformed_records = 0;
    char title_id[16]{};
};

/* FW 9.60 KERN_PROC lookup; returns the last non-self eboot.bin process. */
[[nodiscard]] ProcessLookupResult find_game_process() noexcept;

} // namespace prefbar::ps5
