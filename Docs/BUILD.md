# Building PrefBar

This document describes the build workflow for the current 0.2.0 source tree. The PS5 Payload SDK is installed separately from the repository. PrefBar does not vendor or download the SDK during the build; the compatibility sources needed by the project are already present in the source tree.

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
5. Embeds the renderer ELF into the controller build.
6. Runs static release-artifact verification.
7. Stages the combined controller payload in `dist/` and writes a SHA-256 checksum file when a supported checksum utility is available.

The release payload is:

```text
dist/PrefBar-complete_V0.2.0.elf
```

The renderer and controller ELFs under `build-ps5/` are intermediate artifacts. The combined payload is the file intended for release distribution. Do not commit generated build outputs or release binaries to the source repository; publish the final payload through GitHub Releases.

The release verifier performs static checks on the ELF files, embedded renderer, expected identity/configuration markers, UI/control markers, and selected notification build option. Passing those checks is not a substitute for loading the payload and testing it on a PS5.

## Optional host tests

Host tests do not require the PS5 SDK and do not produce a PS5 payload:

```sh
make test
```

This configures the root CMake project with `PREFBAR_BUILD_PS5=OFF`, builds the test executables under `build-host/`, and runs CTest. The tests cover the telemetry wire format, diagnostic-event/configuration behavior, and touchpad triple-click helper.

## Build options

System-toast notification code can be excluded from the PS5 build:

```sh
PREFBAR_DEBUG_NOTIFY=OFF make payload
```

This controls the optional notification implementation at build time. Runtime notification preferences are separately configured in `/data/prefbar/prefbar.conf`.

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