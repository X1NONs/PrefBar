# Credits and provenance

PrefBar has its own controller organization, telemetry packet, telemetry worker integration, and current overlay data path. Some low-level PS5 integration code remains derivative of earlier open-source work. Existing file-level copyright headers and SPDX identifiers are retained.

- **Common FPS for PS5** — porhe911, GPL-3.0-or-later: portions of the ShellUI loader, hook/controller integration and renderer base. https://github.com/porhe911/Common-FPS-for-PS5
- **SimpleFPS** — khalifa007, GPL-3.0-or-later: inherited PS5 firmware compatibility changes. https://github.com/khalifa007/SimpleFPS
- **ps5-dualsense-overlay** — erickdavestech, GPL-3.0-or-later: inherited renderer and injection-flow work. Controller artwork and its renderer path have been removed from PrefBar.
- **shsrv** — John Törnblom, GPL-3.0-or-later: portions of the in-tree ptrace implementation and stable ELF-loader bridge. The required source is included in this repository; a separate checkout is not required. Original per-file notices remain in place. https://github.com/ps5-payload-dev/shsrv
- **PS5 Payload SDK** — John Törnblom and contributors: toolchain and system headers. https://github.com/ps5-payload-dev/sdk
- **etaHEN** — LightningMods and contributors: telemetry API/source reference for DCE, CPU, memory and sensor integrations; it is not a build dependency or separately included source checkout. https://github.com/etaHEN/etaHEN

PrefBar-specific organization is centered on `src/ps5/prefbar_controller.cpp`, `src/ps5/telemetry/prefbar_telemetry.c`, `src/ps5/shellui_payload/`, and `include/prefbar/`.
