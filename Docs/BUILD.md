# Building PrefBar

This document describes the build workflow for the current 0.3.1 source tree. The PS5 Payload SDK is installed separately from the repository. PrefBar does not vendor or download the SDK during the build; the compatibility sources needed by the project are already present in the source tree.

## Requirements

For a PS5 payload build:

- A PS5 Payload SDK installation compatible with the target environment.
- The SDK's `bin/prospero-cmake` wrapper.
- The SDK target C++ runtime archives: `libc++.a`, `libc++abi.a`, and `libunwind.a`.
- CMake 3.20 or newer, GNU Make, Bash, and Python 3.
- On macOS, Homebrew LLVM 18 for the SDK wrapper environment (`brew install llvm@18`).

For optional host tests, also install a host C and C++ compiler and the platform's usual thread-development support.

Set the SDK root if it is not installed at the default location:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

`PS5_PAYLOAD_SDK` must point to the SDK root, not to its `bin/` directory. The build script looks for `bin/prospero-cmake` and checks for the three C++ runtime archives below the SDK's `target/` directory.

## Build the combined PS5 payload

Run this from the repository root:

```sh
make payload
```

The root `Makefile` invokes `scripts/scripts/build_ps5.sh`. That script:

1. Checks the SDK wrapper and required C++ runtime archives.
2. On macOS, locates the Homebrew LLVM 18 compiler binaries.
3. Configures the repository's root `CMakeLists.txt` with `prospero-cmake` and `PREFBAR_BUILD_PS5=ON`.
4. Builds the ShellUI renderer ELF and the controller ELF.
5. Embeds the renderer ELF into the controller build and, when `PREFBAR_ENABLE_WEB=ON` (the default), embeds the Web Control Center page, links its HTTP server, and includes the PS5 Media shortcut installer and generated shortcut assets.
6. Runs static release-artifact verification.
7. Stages the combined controller payload in `dist/` and writes a SHA-256 checksum file when a supported checksum utility is available.

With `PREFBAR_ENABLE_WEB=OFF`, the page asset, HTTP server, Media shortcut installer, generated shortcut assets, and AppInstUtil link dependency are omitted; the controller, telemetry worker, and embedded ShellUI renderer are still built. There is no separate app-launcher build flag: the PS5 Media shortcut is included by `PREFBAR_ENABLE_WEB=ON`.

The release payload is:

```text
dist/Prefbar_V0.3.1.elf
```

The renderer and controller ELFs under `build-ps5/` are intermediate artifacts:

- `build-ps5/PrefBar_ShellUI_v0.3.1.elf` — intermediate ShellUI renderer.
- `build-ps5/PrefBar_controller_v0.3.1.elf` — controller containing the embedded renderer and, when enabled, the Web Control Center and Media shortcut integration.

The combined payload is the file intended for release distribution. Do not commit generated build outputs or release binaries to the source repository; publish the final payload through GitHub Releases.

The release verifier performs static checks on the ELF files, embedded renderer, expected identity/configuration markers, UI/control markers, and selected notification build option. With the Web Control Center enabled, it also checks expected web-interface markers. These checks do not exercise PS5 shortcut registration through AppInstUtil on hardware. Passing them is not a substitute for loading the payload and testing it on a PS5.

## Optional host tests

Host tests do not require the PS5 SDK and do not produce a PS5 payload:

```sh
make test
```

This configures the root CMake project with `PREFBAR_BUILD_PS5=OFF`, builds the host test executables under `build-host/`, and runs CTest. The current CMake configuration defines nine host test targets, plus one additional game-process test target on Linux. The suite covers telemetry wire-format validation, diagnostic-event and configuration behavior, telemetry-pool handling, adaptive DCE counter selection, touchpad triple-click handling, ShellUI hook-protocol validation, horizontal layout, crash-strike tracking, game-session handling, and (on Linux) game-process parsing. The hook-protocol test checks firmware/backend selection, EFAULT diagnostic conditions, and validation of the one-byte main-thread-guard request.

## Build options

System-toast notification code can be excluded from the PS5 build:

```sh
PREFBAR_DEBUG_NOTIFY=OFF make payload
```

`PREFBAR_DEBUG_NOTIFY` is still a supported CMake build option in this branch and is enabled by default by the PS5 build script. It controls whether the optional system-toast implementation is compiled. Runtime notification preferences are configured separately in `/data/prefbar/prefbar.conf`.

The embedded Web Control Center can also be excluded from the PS5 build:

```sh
PREFBAR_ENABLE_WEB=OFF make payload
```

This omits the embedded `web/index.html` asset, Control Center HTTP server, app-launcher installer, generated `param.json` and icon assets, and the `SceAppInstUtil` link dependency. The PS5 controller, telemetry worker, and embedded ShellUI renderer are still built. Set `PREFBAR_ENABLE_WEB=ON` (the default) to include the complete Web Control Center and Media shortcut integration.

### PS5 Media shortcut

When `PREFBAR_ENABLE_WEB=ON`, the build also includes a PS5 Media-category shortcut named **PrefBar**. It is a shortcut to the Control Center, not a separate native application package or payload. The generated metadata uses Title ID `PREF00001` and the deep link `http://127.0.0.1:8998/`; the shortcut icon is generated from `Docs/logo.png` and embedded into the controller payload. No separate launcher files need to be installed manually.

At runtime, the controller starts the Control Center HTTP server and then asks the embedded launcher installer to register the shortcut using `SceAppInstUtil`. Selecting the shortcut opens the Control Center served by the already-running PrefBar controller. It does **not** load or start the PrefBar payload. Load the combined payload first; if the Control Center server is not running, the shortcut's local URL is unavailable.

`PREFBAR_LAUNCHER_TITLE_ID` and `PREFBAR_LAUNCHER_PORT` are internal CMake values set by the project when web support is enabled, not separate user-facing build switches. The source checks that the launcher port matches the Control Center's HTTP port.

## Cleanup

```sh
make clean       # removes build-host/ and build-ps5/
make distclean   # also removes dist/
```

## Troubleshooting

- **SDK not found:** set `PS5_PAYLOAD_SDK` to the SDK root and confirm that `bin/prospero-cmake` exists and is executable.
- **C++ runtime archive missing:** confirm that `libc++.a`, `libc++abi.a`, and `libunwind.a` are installed somewhere under the SDK's `target/` directory.
- **macOS LLVM error:** install Homebrew LLVM 18 with `brew install llvm@18`, then retry. The script sets `LLVM_BINDIR` and `LLVM_CONFIG` when it can locate the Homebrew installation.
- **Release verification failure:** inspect the first failed check and the two intermediate ELF files. Do not bypass a failed check without identifying its cause.
- **Payload builds but does not run as expected:** a successful cross-build does not prove firmware/runtime compatibility. Record the PS5 firmware, payload-loader environment, and PrefBar logs when reporting a hardware issue.
