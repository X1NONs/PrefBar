#include "prefbar/shellui_hook_protocol.hpp"

#include <cassert>
#include <iostream>

int main() {
    using prefbar::ShellUiHookBackend;
    using prefbar::shellui_hook_probe_backend;
    using prefbar::shellui_hook_probe_needs_mdbg_diagnostic;

    for (const std::uint32_t sdk : {0x09050005U, 0x10010000U}) {
        assert(shellui_hook_probe_backend(sdk) == ShellUiHookBackend::Mdbg);
        assert(prefbar::shellui_main_thread_guard_backend(sdk) ==
               ShellUiHookBackend::Mdbg);
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, EFAULT, 0));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, -1, EFAULT));
        assert(prefbar::shellui_hook_backend(sdk, false) ==
               ShellUiHookBackend::Unsupported);
    }

    for (const std::uint32_t sdk : {0x08300000U, 0x09600000U, 0x10000000U}) {
        assert(shellui_hook_probe_backend(sdk) == ShellUiHookBackend::PtraceIo);
        assert(shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, EFAULT, 0));
        assert(shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, -1, EFAULT));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, false, EFAULT, 0));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, 0, EFAULT));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, -1, EPERM));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, EIO, 0));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, -1, 0));
    }

    for (const std::uint32_t sdk : {0U, 0x08200000U, 0x09000000U,
                                   0x11000000U, 0x14000000U}) {
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, EFAULT, 0));
        assert(!shellui_hook_probe_needs_mdbg_diagnostic(sdk, true, -1, EFAULT));
    }

    assert(prefbar::shellui_main_thread_guard_backend(0x09600000U) ==
           ShellUiHookBackend::Unsupported);
    assert(prefbar::shellui_hook_backend(0x09600000U, true) ==
           ShellUiHookBackend::PtraceIo);

       prefbar::ShellUiHookRequest guard{};
       guard.pid = 58;
       guard.nonce = 1;
       guard.method_address = 0x837bcb9d0ULL;
       guard.hook_address = guard.method_address;
       guard.displaced_size = 1;
       std::memset(guard.expected, 0x90, sizeof(guard.expected));
       std::memcpy(guard.desired, guard.expected, sizeof(guard.desired));
       guard.desired[0] = 0xc3;
       guard.checksum = prefbar::shellui_hook_request_checksum(guard);
       assert(prefbar::shellui_main_thread_guard_request_is_valid(guard, 58));
       assert(!prefbar::shellui_main_thread_guard_request_is_valid(guard, 59));
       guard.desired[1] = 0xcc;
       guard.checksum = prefbar::shellui_hook_request_checksum(guard);
       assert(!prefbar::shellui_main_thread_guard_request_is_valid(guard, 58));
       guard.desired[1] = guard.expected[1];
       guard.desired[0] = guard.expected[0];
       guard.checksum = prefbar::shellui_hook_request_checksum(guard);
       assert(!prefbar::shellui_main_thread_guard_request_is_valid(guard, 58));

       std::cout << "PrefBar hook backend and guard protocol test: PASS\n";
}