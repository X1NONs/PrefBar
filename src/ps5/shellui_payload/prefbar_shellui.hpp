/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdarg>

namespace prefbar::ps5::shellui {

/* Persist the last reached startup stage even when /data logging fails. */
void record_stage(const char* stage) noexcept;

/* Resolve the already-loaded ShellUI Mono runtime and PUI assemblies. */
bool initialize_runtime();

/* Bind the loopback UDP receiver used by the controller. */
bool initialize_receiver();

/* Remain blocked on IPC for the complete SceShellUI process lifetime. */
[[noreturn]] void run_receiver_loop();

} // namespace prefbar::ps5::shellui
