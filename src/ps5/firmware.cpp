/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps5_platform.hpp"

#include <sys/sysctl.h>

namespace prefbar::ps5 {

std::uint32_t firmware_sdk_version() noexcept {
    std::uint32_t version = 0;
    std::size_t size = sizeof(version);
    if (sysctlbyname("kern.sdk_version", &version, &size, nullptr, 0) != 0 ||
        size != sizeof(version)) {
        return 0;
    }
    return version;
}

} // namespace prefbar::ps5
