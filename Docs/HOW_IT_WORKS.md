# PrefBar: how it works and how to build

This document describes the current implementation and the build flow. It is intentionally a source-review document, not a feature roadmap.

## Build model

The repository has one CMake entry point at its root. There is no separate `ps5/` CMake project.

The PS5 SDK is the only external **project/toolchain** dependency. PrefBar's required compatibility sources are already present under `src/ps5/compat/`; the build does not clone or fetch CommonFPS, shsrv, or etaHEN source trees. The PS5 SDK's static C++ runtime libraries must be installed in its target sysroot.

### Host checks

Host tests are optional and do not produce a PS5 payload:

```sh
make test
```

The test sources remain in `scripts/tests/`. Test executables are created only in `build-host/`.

### PS5 build

Set the SDK path if it is not installed at the default location:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Build the release payload:

```sh
make payload
```

The build script configures the **repository root** with the SDK's `prospero-cmake`, setting `PREFBAR_BUILD_PS5=ON`. It then builds the renderer and controller targets, embeds the renderer ELF into the controller, verifies the two intermediate artifacts, and stages only the combined controller ELF in `dist/`.

Disable optional system-toast notifications with:

```sh
PREFBAR_DEBUG_NOTIFY=OFF make payload
```

Clean generated files:

```sh
make clean       # build-host/ and build-ps5/
make distclean   # also removes dist/
```

## Build outputs

The PS5 CMake configuration defines two targets:

1. `prefbar_shellui` creates `PrefBar_ShellUI_v0.2.0.elf`, the renderer image.
2. `prefbar_controller` creates `PrefBar_controller_v0.2.0.elf`, the controller payload.

The helper `scripts/tools/embed_binary.py` converts the renderer ELF into a generated C++ byte array under `build-ps5/generated/`. That generated source is linked into the controller. `scripts/tools/verify_release.py` checks ELF signatures, embedding, PrefBar identity/runtime markers, expected UI/control markers, and the selected notification configuration.

The single release artifact is:

```text
dist/PrefBar-complete_V0.2.0.elf
```

The renderer ELF and controller ELF in `build-ps5/` are intermediate build products, not separate release payloads.

## Runtime configuration

On first start, PrefBar creates `/data/prefbar/prefbar.conf` if it does not already exist. It never overwrites an existing user file. Values are simple `key=value` entries; blank lines, comments beginning with `#` or `;`, and section labels are ignored. Invalid or out-of-range values fall back to defaults. Changes take effect after restarting PrefBar.

Supported settings include `notifications`, `startup_notification`, `error_notifications`, `debug_notifications`, `debug_notify`, `notify_sec`, `overlay`, `text_color` (`#RRGGBB`), `text_shadow`, `overlay_mode`, `overlay_font`, `overlay_corner`, `overlay_x`, `overlay_y`, `sensors`, `show_fps`, `show_cpu`, `show_soc_temp`, `show_fan_duty`, `show_memory`, `touchpad_toggle`, `diag`, and `poll_us`.
Boolean settings use `0` or `1`. `debug_notify` accepts `0`–`4`; `notify_sec` accepts `0`–`3600` (zero disables periodic performance toasts); `overlay_mode` is `0` (compact) or `1` (debug rows); `overlay_font` accepts `10`–`96`; `overlay_corner` is `0` (top-left) or `2` (bottom-left); `overlay_x` accepts `0`–`1800`; `overlay_y` accepts `0`–`1000`; and `poll_us` accepts `500`–`20000`. Fan telemetry is fan-duty percentage, not RPM.

The overlay color and shadow are carried in the version-2 telemetry packet. The touchpad triple-click can be disabled in configuration without disabling telemetry. `overlay=0` hides the renderer output while the telemetry worker and status files continue running.

Example configuration:

```ini
[notifications]
notifications=1
startup_notification=1
error_notifications=1
debug_notifications=1
debug_notify=3
notify_sec=0

[overlay]
overlay=1
text_color=#F2F2F2
text_shadow=1
overlay_mode=0
overlay_font=26
overlay_corner=0
overlay_x=24
overlay_y=24

[telemetry]
sensors=1
show_fps=1
show_cpu=1
show_soc_temp=1
show_fan_duty=1
show_memory=1

[input]
touchpad_toggle=1

[diagnostics]
diag=0
poll_us=2000
```

## Runtime architecture

### 1. Controller and lifecycle

`src/ps5/prefbar_controller.cpp` is the main controller entry point. It creates the runtime data directory, requests the main thread name `PrefBar` through `SYS_thr_set_name`, starts the in-process telemetry worker, observes ShellUI and game processes, and coordinates renderer injection. The startup log records the return code and errno for the thread-name request; confirm how the active-process viewer presents it on the target console. Process observations use stability checks so a single transient process-list result does not immediately trigger renderer startup.

### 2. Telemetry worker

`src/ps5/telemetry/prefbar_telemetry.c` runs inside the controller. It gathers the metrics supported by the current PS5 integration, formats the overlay rows, maintains diagnostic/status output under `/data/prefbar`, and sends the row packet to the local renderer over UDP on loopback port `39028`.

The shared packet layout and validation functions are in `include/prefbar/telemetry_wire.h`. It is a C-compatible header shared by the telemetry worker and C++ renderer. Version 2 carries overlay visibility, touchpad-toggle and text-shadow flags, plus an RGB text color. The separate overlay control messages use the protocol declarations in `include/prefbar/shellui_hook_protocol.hpp` and the control port defined by the PrefBar protocol.

### 3. ShellUI renderer

`src/ps5/shellui_payload/prefbar_shellui.cpp` binds to the ShellUI Mono/PUI runtime, receives and validates telemetry packets, and draws PrefBar's text rows. `prefbar_shellui_entry.cpp` provides the renderer entry point.

The controller artwork, embedded pad image data, sprite creation, PNG writing, and controller-image overlay have been removed. The renderer still reads the physical touchpad click state because triple-click is the current UI visibility control. That input path is separate from the removed artwork path: it sends an overlay on/off control message and does not draw a controller image.

### 4. PS5 integration and loader

The other files under `src/ps5/` provide firmware identification, game-process discovery, remote calls, ShellUI injection, hook coordination, and the bridge that loads the embedded renderer. The public declarations for platform discovery are grouped in `src/ps5/ps5_platform.hpp`; remote-call and injection interfaces are grouped in `src/ps5/ps5_injection.hpp`.

`src/ps5/compat/pt.c` contains the ptrace I/O compatibility functions used by the existing injector. `src/ps5/stable_elfldr_bridge.c` contains the in-tree renderer ELF-loading bridge. Both are bundled in this source tree, so there is no separate shsrv checkout or `.deps/` directory. The unused legacy ELF-loader implementation has been removed. The remaining derived code retains its original file notices; bundling it removes the external source-checkout requirement, but does not make that code a clean-room PrefBar rewrite.

## Repository layout

The existing directory layout is intentional and is not rearranged by the build:

| Path | Role |
|---|---|
| `CMakeLists.txt` | Single CMake entry point for host tests and PS5 targets |
| `Makefile` | Short commands for tests, payload build and cleanup |
| `include/prefbar/` | Shared protocol and input helpers |
| `src/ps5/` | Controller, PS5 integration, telemetry, loader, and renderer |
| `scripts/tests/` | Host-side self-tests |
| `scripts/tools/` | Binary embedding and release verification helpers |
| `scripts/scripts/` | PS5 build entry-point script |
| `Docs/` | Project notes and this document |

## Review boundaries

The source layout keeps project-specific code under `src/ps5/`, shared interfaces under `include/prefbar/`, and developer utilities in their existing nested `scripts/` directories. The controller-artwork feature and GitHub automation have been removed. Small PS5 declarations are grouped by subsystem rather than kept in one-header-per-source-file form. It does not claim that the inherited PS5 injector, hook, renderer runtime, or compatibility loader has been rewritten from scratch. The hook-probe log now reports the wrapper return, raw ptrace syscall return, whether the read was attempted, immediate errno, returned `piod_len` versus the requested byte count, and whether a write was attempted. For example, a probe-only request should say `write_attempted=0`; `write_rc=-1` by itself is not evidence of a failed write. For the `ptrace-io` backend, `read_reported_len` is the length value returned in `ptrace_io_desc.piod_len`; for MDBG it is the requested size on success and zero on failure. Interpret it alongside `read_raw_rc` and `read_errno`, especially when the read fails. This is diagnostic instrumentation only: it does not claim to fix the reported FW 9.05/10.01 probe failure or alter the existing hook behavior. The first validation step remains a real SDK build followed by an on-console test before treating the candidate as stable.
