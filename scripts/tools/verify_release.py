#!/usr/bin/env python3
# Copyright (C) 2026 X1NONs
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks for the combined PrefBar PS5 release payload."""

from __future__ import annotations

import os
import pathlib
import sys


def enabled(name: str, default: str = "ON") -> bool:
    return os.environ.get(name, default).upper() in {"ON", "1", "TRUE", "YES"}


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: verify_release.py controller.elf shellui_renderer.elf",
            file=sys.stderr,
        )
        return 2

    controller_path, renderer_path = map(pathlib.Path, sys.argv[1:3])
    try:
        controller = controller_path.read_bytes()
        renderer = renderer_path.read_bytes()
    except OSError as exc:
        print(f"ERROR: could not read release artifact: {exc}", file=sys.stderr)
        return 2

    checks = [
        (controller.startswith(b"\x7fELF"), "controller is not an ELF"),
        (renderer.startswith(b"\x7fELF"), "ShellUI renderer is not an ELF"),
        (len(renderer) > 4096, "ShellUI renderer ELF is unexpectedly small"),
        (controller.find(renderer) > 0, "exact renderer ELF is not embedded in controller"),
        (b"PrefBar V0.3.1 controller" in controller, "PrefBar controller identity marker missing"),
        (b"/data/prefbar" in controller, "PrefBar runtime data path missing"),
        (b"game.session.start" in controller and b"game.exited" in controller,
         "game-session lifecycle markers missing"),
        (b"renderer.online" in controller, "renderer lifecycle marker missing"),
        (b"process.name" in controller and b"main thread name request" in controller,
         "PrefBar main-thread naming request marker missing"),
        (b"/data/prefbar/prefbar.conf" in controller,
         "PrefBar runtime configuration path missing"),
        (b"read_raw_rc=%d" in controller and b"read_attempted=%d" in controller and b"read_reported_len=%u" in controller and b"read_requested=%u" in controller,
         "expanded hook-probe diagnostic markers missing"),
        (b"text_color=#F2F2F2" in controller and b"text_shadow=1" in controller,
         "default PrefBar appearance configuration missing"),
        (b"id_prefbar_shadow" in renderer,
         "optional text-shadow renderer path missing"),
        (b"FAN %d%%" in controller and b"fan_duty=%d\n" in controller,
         "fan-duty overlay/status fields missing"),
        (b"prefbar: rows built" in renderer, "PrefBar telemetry row renderer marker missing"),
        (b"PBCTL1:OVERLAY:ON" in renderer and b"PBCTL1:OVERLAY:OFF" in renderer,
         "touchpad overlay control channel missing"),
        (b"file:///data" not in renderer,
         "renderer contains a file:///data reference"),
        (b"cxml://psnotification" not in renderer,
         "renderer must not send system notifications"),
        ((b"cxml://psnotification" in controller) == enabled("PREFBAR_DEBUG_NOTIFY"),
         "debug notification code does not match PREFBAR_DEBUG_NOTIFY"),
        ((b"PrefBar V0.3.1 by X1NONs" in controller) == enabled("PREFBAR_DEBUG_NOTIFY"),
         "versioned startup attribution toast does not match PREFBAR_DEBUG_NOTIFY"),
    ]

    if enabled("PREFBAR_ENABLE_WEB"):
        checks.extend([
            (
                b"PS5_USER_AGENT_PATTERN" in controller and
                b"PS5_CONSOLE_MODE" in controller,
                "PS5 native User-Agent detection or console styling missing",
            ),
            (
                b".game-scene { position: absolute; z-index: 0;" in controller and
                b".scene-shade { position: absolute; z-index: 1;" in controller,
                "PS5-safe preview image stacking missing",
            ),
            (
                b"data:image/jpeg;base64," in controller,
                "embedded F-15 preview image missing from Control Center",
            ),
        ])

    checks.extend([
        (
            b"background Panel factory failed" in renderer and
            b"background Panel recovered without rebuilding telemetry rows" in renderer,
            "background panel diagnostics or independent recovery path missing",
        ),
        (
            b"optional BackgroundStyle setter unavailable" in renderer,
            "optional PUI BackgroundStyle compatibility path missing",
        ),
        (
            b"background Panel property probe" in renderer and
            b"background Panel shown" in renderer,
            "background panel property probe or shown-state diagnostics missing",
        ),
        (
            b"prefbar: text applied cells" in renderer,
            "per-reading cell layout marker missing from renderer",
        ),
        (
            b"/dev/icc_fan" in controller and
            b"fan_target_temperature" in controller and
            b"0xC01C8F08" in controller and b"0xC01C8F07" in controller,
            "optional automatic fan target controller path missing",
        ),
    ])

    failed = [message for ok, message in checks if not ok]
    for message in failed:
        print(f"ERROR: {message}", file=sys.stderr)
    if failed:
        return 1

    print("PrefBar release artifact checks: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
