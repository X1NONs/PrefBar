# PrefBar 0.2.0: how it works

This document describes the implementation present in the repository. It is not a feature roadmap and does not claim that every firmware path has been tested.

## Build model

The repository has one CMake entry point: the root `CMakeLists.txt`. There is no separate PS5 CMake project.

The PS5 Payload SDK supplies the cross-compilation toolchain and PS5 headers/libraries. The project-specific compatibility sources are bundled under `src/ps5/compat/`; the build does not clone separate Common FPS, shsrv, or etaHEN source trees. Some bundled source remains derived from those projects. See [CREDITS.md](CREDITS.md) and the original notices in the source files.

### Build targets

The PS5 CMake configuration defines two targets:

1. `prefbar_shellui` builds `PrefBar_ShellUI_v0.2.0.elf`, the renderer image.
2. `prefbar_controller` builds `PrefBar_controller_v0.2.0.elf`, the controller payload.

`scripts/tools/embed_binary.py` converts the renderer ELF into a generated C++ byte array under `build-ps5/generated/`. That generated source is linked into the controller. `scripts/tools/verify_release.py` performs static checks on the intermediate artifacts. The build script stages one combined payload:

```text
dist/PrefBar-complete_V0.2.0.elf
```

The renderer ELF and controller ELF under `build-ps5/` are intermediate build outputs, not separate files that users need to load.

### Host checks

Host tests are optional and do not produce a PS5 payload:

```sh
make test
```

The test sources are under `scripts/tests/`; test executables are created under `build-host/`.

## Runtime architecture

### 1. Controller and lifecycle

`src/ps5/prefbar_controller.cpp` is the controller entry point. It creates `/data/prefbar`, starts the embedded telemetry worker, and monitors the running PS5 processes.

The controller asks the system to name its main thread `PrefBar` using `SYS_thr_set_name` and records the return code and `errno`. This is a thread-name request; it does not rename the ELF file or guarantee that every process viewer will display the process as `PrefBar`.

The controller waits for stable process observations before starting the renderer. The current source uses a 50 ms polling interval, a 10-sample ShellUI stability gate, and a 3-sample game-process stability gate. Renderer startup is deferred until both ShellUI and a game process have been observed as stable.

### 2. Telemetry worker

`src/ps5/telemetry/prefbar_telemetry.c` runs as a thread inside the controller payload. It samples the display flip counter through `/dev/dce` when a game process is ready, maintains frame statistics, queries optional system telemetry APIs, writes status/diagnostic files, and sends overlay rows to the renderer.

The telemetry worker does not read or write game-process memory. The DCE counter is used as the source for frame statistics. Sensor APIs are optional/weakly linked, so a metric can be unavailable on a particular firmware or runtime environment.

The current overlay can show:

- FPS, average frametime, and 1% low.
- CPU utilization and CPU temperature when available.
- SoC temperature when available.
- Fan duty as a percentage.
- RAM and VRAM readings when available.

**Fan duty is not fan RPM.** The currently used fan API reports a Q10 duty value, converted to a percentage. The project does not claim that this API supplies tachometer RPM.

The telemetry worker writes human-readable `status.txt` and key/value `status.kv` files under `/data/prefbar`. It also appends debug events to `debug.log`; optional diagnostic captures are written to `diag.txt`.

### 3. Telemetry and control protocol

`include/prefbar/telemetry_wire.h` defines the C-compatible version-2 telemetry packet shared by the C telemetry worker and C++ renderer. The current packet carries the text rows, font size, corner/position, text color, overlay visibility, text-shadow state, and touchpad-toggle state.

The controller sends telemetry to the renderer over UDP loopback on port `39028`. The renderer sends overlay-toggle events back to the controller over UDP loopback on port `39029`. These messages are local to the console; they are not sent over the network interface.

The hook/control request and acknowledgment structures are declared in `include/prefbar/shellui_hook_protocol.hpp`. The stage file path is declared in `include/prefbar/shellui_stage.hpp`.

### 4. ShellUI renderer

`src/ps5/shellui_payload/prefbar_shellui.cpp` is the renderer implementation and `prefbar_shellui_entry.cpp` provides its entry point. The renderer attaches to the ShellUI Mono runtime, uses the PUI UI classes to create text labels, receives telemetry packets, and updates the labels when the packet or visibility state changes.

The renderer is built as an intermediate ELF and embedded in the controller. Users therefore load the combined `PrefBar-complete_V0.2.0.elf`, not a separate renderer ELF.

The renderer reads the DualSense touchpad-click state. Three distinct touchpad-click press edges within a 1.5-second window toggle the overlay. Hiding the overlay does not stop the telemetry worker.

The renderer contains more than one runtime path for integrating its drawing loop with ShellUI. The selected path depends on the runtime and hook compatibility checks. If initialization cannot establish a supported path, it fails closed rather than reporting the renderer as online.

The old controller artwork, embedded pad image, sprite creation, PNG writing, and controller-image overlay are not part of the current renderer. Touchpad input remains because it is the overlay visibility control.

## Runtime configuration

On first start, the telemetry worker creates this file if it does not already exist:

```text
/data/prefbar/prefbar.conf
```

It does not overwrite an existing file. Changes take effect after restarting PrefBar.

The parser reads simple `key=value` lines. Blank lines, comment lines beginning with `#` or `;`, and section labels such as `[overlay]` are ignored. Section labels are for readability only; keys are matched globally, not scoped to a section. Use each key once. Unknown keys and malformed/out-of-range values are ignored and the built-in default is used.

All boolean values use `0` (off) or `1` (on).

| Setting | Meaning | Accepted values / default |
|---|---|---|
| `notifications` | Master system-toast switch | `0` or `1`; default `1` |
| `startup_notification` | Show the delayed startup attribution toast | `0` or `1`; default `1` |
| `error_notifications` | Allow error toasts | `0` or `1`; default `1` |
| `debug_notifications` | Allow non-error debug/milestone toasts | `0` or `1`; default `1` |
| `debug_notify` | Maximum toast verbosity: `0` off, `1` errors, `2` warnings, `3` information/milestones, `4` trace | `0`–`4`; default `3` |
| `notify_sec` | Periodic performance-toast interval; zero disables periodic toasts | `0`–`3600` seconds; default `0` |
| `overlay` | Show or hide the on-screen overlay | `0` or `1`; default `1` |
| `text_color` | Overlay text color | `#RRGGBB`; default `#F2F2F2` |
| `text_shadow` | Draw a text shadow | `0` or `1`; default `1` |
| `overlay_mode` | `0` compact layout, `1` extended diagnostic rows | `0` or `1`; default `0` |
| `overlay_font` | Text font size in pixels | `10`–`96`; default `26` |
| `overlay_corner` | Overlay anchor: `0` top-left, `2` bottom-left | `0` or `2`; default `0` |
| `overlay_x` | Horizontal margin in pixels | `0`–`1800`; default `24` |
| `overlay_y` | Vertical margin in pixels | `0`–`1000`; default `24` |
| `sensors` | Enable optional sensor/CPU/memory sampling | `0` or `1`; default `1` |
| `show_fps` | Show the FPS row | `0` or `1`; default `1` |
| `show_cpu` | Show CPU utilization/temperature | `0` or `1`; default `1` |
| `show_soc_temp` | Show SoC temperature | `0` or `1`; default `1` |
| `show_fan_duty` | Show fan-duty percentage | `0` or `1`; default `1` |
| `show_memory` | Show RAM/VRAM metrics | `0` or `1`; default `1` |
| `touchpad_toggle` | Enable triple-click overlay visibility control | `0` or `1`; default `1` |
| `diag` | Enable diagnostic captures | `0` or `1`; default `0` in the embedded PS5 build |
| `poll_us` | Telemetry polling interval in microseconds | `500`–`20000`; default `2000` |

Example configuration:

```ini
# PrefBar 0.2.0 configuration

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

Setting `overlay=0` hides the renderer output while telemetry and status-file updates continue. Setting `sensors=0` disables the optional sensor sampling; frame-counter sampling remains a separate path. Setting `touchpad_toggle=0` disables the touchpad gesture.

## Diagnostics and hook-probe logs

The controller writes startup information to `/data/prefbar/controller.log`; the injector writes `/data/prefbar/injector.log`. The renderer writes `/system_tmp/prefbar_shellui.log` and records its latest startup stage in `/system_tmp/prefbar_shellui.stage`.

The hook-probe diagnostic record distinguishes the wrapper return (`read_rc`), raw ptrace syscall result (`read_raw_rc`), whether the read was attempted, the captured `errno`, and the requested/reported I/O descriptor length. For a probe-only request, `write_attempted=0`; the default `write_rc=-1` is a sentinel and does not mean that a write failed.

For the ptrace-I/O path, `read_reported_len` is copied from `ptrace_io_desc.piod_len` after the syscall attempt. That field is initialized with the requested size and may remain unchanged if the operation fails, so a non-zero reported length alone does not prove that bytes were transferred. Interpret it together with `read_rc`, `read_raw_rc`, `read_attempted`, and `read_errno`. The MDBG path reports the requested size on success and zero on failure.

This instrumentation improves diagnosis; it is not itself a fix for firmware-specific probe failures. A successful static release check or host test does not establish that every firmware's ShellUI path works. Confirm with a PS5 SDK build and hardware test.

## Repository layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | Single CMake entry point for host tests and PS5 targets |
| `Makefile` | Short commands for host tests, PS5 payload build, and cleanup |
| `include/prefbar/` | Shared telemetry, hook protocol, stage, and touchpad interfaces |
| `src/ps5/prefbar_controller.cpp` | Controller lifecycle and startup |
| `src/ps5/telemetry/` | Telemetry sampling, configuration, status and diagnostic output |
| `src/ps5/shellui_payload/` | Embedded ShellUI renderer |
| `src/ps5/compat/` | Bundled compatibility sources, including ptrace and ELF-loader code |
| `scripts/scripts/` | PS5 build entry-point script |
| `scripts/tools/` | Renderer embedding and release verification helpers |
| `scripts/tests/` | Host-side self-tests |
| `Docs/` | Build, architecture, and provenance documentation |

## Provenance

PrefBar has its own controller organization, telemetry packet, telemetry worker integration, and current overlay data path. Some low-level PS5 integration, loader, and renderer code remains derived from earlier GPL-licensed projects. Bundling the required source means a separate checkout is not needed to build PrefBar; it does not make that code a clean-room rewrite.

In particular, `src/ps5/stable_elfldr_bridge.c` includes the bundled `src/ps5/compat/elfldr.c` implementation and uses its loader definitions/helper functions, including `elfldr_payload_args()`. The legacy ELF-loader source is therefore still present in the build; documentation should not describe it as removed. See [CREDITS.md](CREDITS.md) for project attribution and the retained file-level copyright/SPDX notices for source provenance.