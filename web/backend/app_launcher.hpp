#pragma once

namespace prefbar::launcher {

/* Request installation of the Media-category shortcut for the running
 * Control Center. The request is asynchronous and safe to call on retries. */
void request_install() noexcept;

} // namespace prefbar::launcher
