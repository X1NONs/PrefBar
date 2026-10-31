# Building PrefBar

This document covers the supported local build workflow. PrefBar uses the PS5 Payload SDK as its only external PS5 toolchain/project dependency; the remaining requirements are ordinary host build tools.

## Requirements

- PS5 Payload SDK compatible with the target firmware/build environment.
- CMake 3.20 or newer.
- GNU Make and Bash.
- Python 3.
- A host C/C++ toolchain for optional host tests.
- The SDK's C++ runtime archives: `libc++.a`, `libc++abi.a`, and `libunwind.a`.
- On macOS, Homebrew LLVM 18 for the SDK wrapper environment.

Set the SDK root if it is not installed at the default path:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

The variable must point to the SDK root, not its `bin/` directory. The build expects `bin/prospero-cmake` beneath it.

## Build the PS5 payload

From the repository root:

```sh
make payload
```

This invokes `scripts/scripts/build_ps5.sh`. The script checks the SDK and runtime archives, configures the root `CMakeLists.txt` using `prospero-cmake`, builds the ShellUI renderer and controller, embeds the renderer ELF into the controller, and runs release verification.

The only release file staged by the build is:

```text
dist/PrefBar-complete_V0.2.0.elf
```

The renderer ELF and controller ELF under `build-ps5/` are intermediate outputs. Do not commit generated binaries; publish the final ELF through GitHub Releases. Binaries are not part of the source tree.

## Optional host tests

Host tests do not require the PS5 SDK:

```sh
make test
```

They are built under `build-host/` and cover the telemetry wire format, diagnostic worker behavior, and touchpad triple-click helper.

## Build options

Disable optional system-toast notifications:

```sh
PREFBAR_DEBUG_NOTIFY=OFF make payload
```

File-based diagnostics remain available; the default `diag=0` avoids exploratory sensor probes in the embedded runtime.

## Cleanup

```sh
make clean       # remove build-host/ and build-ps5/
make distclean   # also remove dist/
```

## Troubleshooting

- **SDK not found:** set `PS5_PAYLOAD_SDK` to the SDK root and confirm `bin/prospero-cmake` exists.
- **Missing C++ runtime archive:** install the required archives (`libc++.a`, `libc++abi.a`, `libunwind.a`) into the SDK target sysroot.
- **macOS LLVM error:** install Homebrew LLVM 18 (`brew install llvm@18`) and retry.
- **Release verification failure:** inspect the first failed check. Verification intentionally checks both ELF outputs, the embedded renderer image, and PrefBar markers; do not bypass a failure without identifying its cause.
