/* PrefBar PS5 injection and remote-execution interfaces. */
#pragma once

#include "prefbar/shellui_hook_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace prefbar::ps5 {

/* Call a function while the target is already ptrace-attached. */
[[nodiscard]] long call_until_breakpoint_on_target_stack(
    pid_t pid, std::uintptr_t address,
    std::uint64_t arg0 = 0, std::uint64_t arg1 = 0,
    std::uint64_t arg2 = 0, std::uint64_t arg3 = 0,
    std::uint64_t arg4 = 0, std::uint64_t arg5 = 0) noexcept;

/* ShellUI discovery and renderer startup. */
bool ensure_shellui_renderer();
[[nodiscard]] pid_t observe_shellui_pid() noexcept;
[[nodiscard]] pid_t shellui_renderer_pid() noexcept;

enum class ShellUiHookPollResult { NoRequest, Applied, Failed };

struct ShellUiHookPatchReport {
    ShellUiHookStatus status = ShellUiHookStatus::InvalidRequest;
    std::uint32_t sdk_version = 0;
    std::uint64_t method_address = 0;
    std::uint32_t displaced_size = 0;
    bool eta_chain = false;
    const char* backend = "none";
    int read_rc = -1;             /* wrapper return */
    int read_raw_rc = -1;         /* PT_IO/MDBG return before cleanup */
    int read_errno = 0;           /* errno captured immediately after read */
    std::uint32_t read_reported_len = 0;
    std::uint32_t read_requested = 0;
    bool read_attempted = false;
    int write_rc = -1;
    bool write_attempted = false;
    bool expected_matched = false;
    bool verified = false;
    bool restored = false;
    bool detached = false;
    bool auth_restored = false;
    bool probe = false;
    bool main_thread_guard = false;
    std::array<std::uint8_t, kShellUiHookPatchSize> observed{};
};

void clear_shellui_hook_protocol_files() noexcept;
[[nodiscard]] ShellUiHookPollResult poll_and_apply_shellui_hook(
    pid_t shellui_pid, ShellUiHookPatchReport& report) noexcept;

struct StableInjectionResult {
    bool auth_changed = false;
    bool auth_restored = false;
    bool attached = false;
    bool elf_loaded = false;
    bool payload_args_ready = false;
    bool remote_stager_ready = false;
    bool remote_functions_ready = false;
    bool target_stack_preserved = false;
    bool bootstrap_started = false;
    bool pthread_create_ok = false;
    bool detached = false;
    bool trace_continue_seen = false;
    bool trace_stop_seen = false;
    int pthread_create_rc = -1;
    unsigned imports_resolved = 0;
    unsigned imports_unresolved = 0;
    const char* first_unresolved = nullptr;
};

[[nodiscard]] StableInjectionResult inject_shellui_stable(
    pid_t shellui_pid, const std::uint8_t* renderer_elf,
    std::size_t renderer_size) noexcept;

} // namespace prefbar::ps5
