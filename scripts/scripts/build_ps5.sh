#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
PREFBAR_DEBUG_NOTIFY="${PREFBAR_DEBUG_NOTIFY:-ON}"

if [[ ! -x "${PS5_PAYLOAD_SDK}/bin/prospero-cmake" ]]; then
  echo "ERROR: PS5 Payload SDK not found at ${PS5_PAYLOAD_SDK}" >&2
  echo "Install the PS5 Payload SDK and set PS5_PAYLOAD_SDK to its root directory." >&2
  exit 1
fi

# SDK C++ runtimes are not present in every SDK installation by default.
for lib in libc++.a libc++abi.a libunwind.a; do
  if [[ ! -d "${PS5_PAYLOAD_SDK}/target" ]] || ! find "${PS5_PAYLOAD_SDK}/target" -type f -name "${lib}" -print -quit | grep -q .; then
    echo "ERROR: SDK runtime library ${lib} is missing." >&2
    echo "Install the required C++ runtime archives in the SDK target sysroot." >&2
    exit 1
  fi
done

# Homebrew's LLVM wrapper setup is required only for macOS hosts.
if [[ "$(uname -s)" == "Darwin" ]]; then
  if [[ -z "${LLVM_BINDIR:-}" ]] && command -v brew >/dev/null 2>&1; then
    LLVM_PREFIX="$(brew --prefix llvm@18 2>/dev/null || true)"
    if [[ -n "${LLVM_PREFIX}" ]]; then
      export LLVM_BINDIR="${LLVM_PREFIX}/bin"
    fi
  fi
  if [[ -n "${LLVM_BINDIR:-}" && -z "${LLVM_CONFIG:-}" && -x "${LLVM_BINDIR}/llvm-config" ]]; then
    export LLVM_CONFIG="${LLVM_BINDIR}/llvm-config"
  fi
  if [[ -z "${LLVM_BINDIR:-}" || ! -x "${LLVM_BINDIR}/clang" || ! -x "${LLVM_BINDIR}/clang++" ]]; then
    echo "ERROR: cannot locate Homebrew LLVM 18 clang binaries." >&2
    echo "Install with: brew install llvm@18" >&2
    exit 1
  fi
fi

"${PS5_PAYLOAD_SDK}/bin/prospero-cmake" \
  -S "${ROOT}" \
  -B "${ROOT}/build-ps5" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPREFBAR_BUILD_PS5=ON \
  -DBUILD_TESTING=OFF \
  -DPREFBAR_DEBUG_NOTIFY="${PREFBAR_DEBUG_NOTIFY}"

if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
else
  JOBS=2
fi
cmake --build "${ROOT}/build-ps5" -j"${JOBS}"

CONTROLLER="${ROOT}/build-ps5/PrefBar_controller_v0.2.0.elf"
RENDERER="${ROOT}/build-ps5/PrefBar_ShellUI_v0.2.0.elf"
for artifact in "${CONTROLLER}" "${RENDERER}"; do
  if [[ ! -s "${artifact}" ]]; then
    echo "ERROR: expected build artifact is missing or empty: ${artifact}" >&2
    exit 1
  fi
done

PREFBAR_DEBUG_NOTIFY="${PREFBAR_DEBUG_NOTIFY}" \
  python3 "${ROOT}/scripts/tools/verify_release.py" "${CONTROLLER}" "${RENDERER}"

OBJDUMP_TOOL="${OBJDUMP:-}"
if [[ -z "${OBJDUMP_TOOL}" ]]; then
  for candidate in \
    "${PS5_PAYLOAD_SDK}/bin/prospero-objdump" \
    "$(command -v llvm-objdump 2>/dev/null || true)" \
    "$(command -v objdump 2>/dev/null || true)"; do
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
      OBJDUMP_TOOL="${candidate}"
      break
    fi
  done
fi
if [[ -n "${OBJDUMP_TOOL}" ]]; then
  if "${OBJDUMP_TOOL}" -d --disassemble-symbols=main "${CONTROLLER}" | grep -q '<fork>'; then
    echo "ERROR: main still calls fork" >&2
    exit 1
  fi
else
  echo "WARNING: no objdump-compatible tool found; skipped main/fork disassembly check." >&2
fi

OUTPUT_NAME="PrefBar-complete_V0.2.0.elf"
DIST="${ROOT}/dist"
mkdir -p "${DIST}"
TEMP_OUTPUT="${DIST}/.${OUTPUT_NAME}.tmp"
cp "${CONTROLLER}" "${TEMP_OUTPUT}"
if [[ ! -s "${TEMP_OUTPUT}" ]]; then
  rm -f "${TEMP_OUTPUT}"
  echo "ERROR: failed to stage final payload." >&2
  exit 1
fi

find "${DIST}" -maxdepth 1 -type f \
  \( -name '*.elf' -o -name '*.plugin' -o -name 'SHA256SUMS.txt' \) \
  ! -name ".${OUTPUT_NAME}.tmp" -delete
mv "${TEMP_OUTPUT}" "${DIST}/${OUTPUT_NAME}"

if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "${DIST}/${OUTPUT_NAME}" | tee "${DIST}/SHA256SUMS.txt"
elif command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "${DIST}/${OUTPUT_NAME}" | tee "${DIST}/SHA256SUMS.txt"
else
  echo "WARNING: neither sha256sum nor shasum found; skipped checksum display." >&2
fi

printf '\nRelease payload: %s\n' "${DIST}/${OUTPUT_NAME}"
echo "dist/ contains the single combined PrefBar payload."
