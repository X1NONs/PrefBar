# Contributing to PrefBar

Thanks for your interest in contributing to PrefBar!

PrefBar is a standalone performance overlay for jailbroken PlayStation 5 consoles. Contributions are welcome across its controller, telemetry system, ShellUI renderer, firmware compatibility, configuration, testing, and documentation.

The priority is reliable, maintainable code that fits the project's existing architecture and works in real PS5 environments.

## Before you start

- Check existing issues and discussions before opening a new one. Avoid duplicate reports and work that is already being addressed.
- For new features, significant behavior changes, or architectural work, open an issue or discussion first. This gives us a chance to discuss the design and potential compatibility concerns before implementation.
- Small bug fixes, documentation improvements, and focused test additions can generally go straight to a pull request.
- For firmware-specific issues, include the exact firmware version and enough detail to reproduce the problem.

## Development setup

PrefBar uses C17 and C++20. The host-side test suite does not require the PS5 Payload SDK.

See [Docs/BUILD.md](Docs/BUILD.md) for the complete build requirements and setup instructions.

To configure, build, and run the host tests from the repository root:

```sh
make test
```

To build the combined PS5 payload, install a compatible PS5 Payload SDK, set `PS5_PAYLOAD_SDK` to the SDK root directory, and run:

```sh
make payload
```

The combined payload is staged under `dist/`. Do not commit generated build files, intermediate ELFs, or release binaries.

## Code contributions & AI policy

Keep changes focused, consistent with the existing implementation, and limited to what is necessary to solve the problem.

**AI tools are permitted, but responsibility for the submitted code remains entirely with the contributor.** You must review, understand, and be able to explain every change in your pull request. AI-generated code must be checked against the actual source, APIs, build environment, and expected runtime behavior before submission.

Submissions containing unverified generated code, hallucinated APIs, generic AI-generated comments, or large unexplained refactors may be rejected or closed. Using an AI tool does not replace engineering judgment, testing, or responsibility for the result.

Additional engineering requirements:

- Preserve existing copyright notices, attribution, and SPDX identifiers. Some low-level integration code is derived from other open-source projects; see [Docs/CREDITS.md](Docs/CREDITS.md).
- Keep C and C++ changes compatible with the project's C17 and C++20 standards.
- Add or update tests when changing behavior, configuration parsing or migration, packet validation, or controller/renderer protocol handling.
- Preserve compatibility with existing configuration files. New settings must have sensible defaults and must not silently overwrite existing user values.
- Treat firmware-dependent APIs and telemetry as potentially unavailable. Unsupported or invalid readings must be handled safely rather than presented as valid measurements or allowed to break the overlay.
- Keep changes to telemetry packets and controller/renderer protocols deliberate. Update protocol versions, structure sizes, layout checks, and relevant tests when required.
- Avoid unrelated refactoring, generated files, debug leftovers, and unnecessary formatting changes in focused fixes.

## Testing and validation

Run `make test` before submitting code changes. Include relevant build and test results in your pull request.

For PS5-specific changes, also run `make payload` when the SDK is available and the change affects the PS5 build.

**A successful host test or cross-build does not prove that a change works on a physical PS5. Compiling successfully is not the same as runtime validation.**

When testing on hardware, provide:

- The exact PS5 firmware version.
- The PrefBar version or commit being tested.
- The payload loader and relevant runtime environment, where applicable.
- The expected behavior and the behavior actually observed.
- Relevant logs or screenshots, with private information removed.

If you have not tested on a physical console, state that clearly. Do not describe untested firmware or behavior as confirmed working.

## Branch workflow

The project uses two primary branches:

- `main` is the stable/release branch.
- `NextUpdate` is the active development branch for the next release.

Contributors should base their work on the current `NextUpdate` branch and open pull requests against `NextUpdate`, unless a maintainer directs them to use another base branch.

The `NextUpdate` branch is **recreated for each development cycle**. When the work for a release is ready, `NextUpdate` is merged into `main`. The old `NextUpdate` branch is then deleted, and a fresh branch with the same name is created from the updated `main`.

This means the branch name stays the same, but its history is reset to the new development baseline after each merge. Before opening a pull request, make sure your work is based on the current incarnation of `NextUpdate`.

## Pull requests

1. Fork the repository and create a focused branch from the current `NextUpdate`.
2. Implement your changes and add or update tests where appropriate.
3. Run the relevant checks and review your complete diff before submitting.
4. Open a pull request targeting `NextUpdate`, unless a maintainer directs you to use another base branch.

A good pull request description should explain:

- What changed and why the change is needed.
- Which issue or discussion it addresses, if applicable.
- Which components or behaviors are affected.
- Which tests and builds you ran, including any that could not be completed.
- Whether the change was tested on a physical PS5 and, if so, on which firmware.

Screenshots are appreciated for visual changes. For firmware compatibility or runtime fixes, include relevant sanitized logs and reproduction details.

## Bug reports

A useful bug report should include the PrefBar version, exact PS5 firmware, steps to reproduce, expected behavior, actual behavior, and relevant logs.

Before publishing logs, remove account details, console identifiers, IP addresses, tokens, and other private information. Only include files and diagnostic information relevant to the issue.

## License

PrefBar is distributed under the **GNU General Public License version 3 (GPL-3.0)**. Only submit work you have the right to contribute and that can be distributed under the project's license. Preserve all applicable third-party copyright notices, attribution, and licensing requirements.

Thanks for helping improve PrefBar!
