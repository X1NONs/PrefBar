/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * PrefBar web Control Center server.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace prefbar::web {

inline constexpr unsigned short kDefaultPort = 8998;

/* Starts the Control Center HTTP server.
 * Returns 0 on success or a positive error code on failure. */
int start() noexcept;

/* Stops the HTTP server and joins its worker thread. */
void stop() noexcept;

/* Returns true while the HTTP server worker is active. */
bool is_running() noexcept;

} // namespace prefbar::web
