/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Host tests for PS5 KERN_PROC parsing and application process grouping.
 */
#include "ps5_platform.hpp"

#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
constexpr long kMockSysctlSyscall = 202;
constexpr std::size_t kPidOffset = 72;
constexpr std::size_t kCommOffset = 447;

struct MockAppInfo {
    std::uint32_t app_id = 0;
    std::uint64_t unknown1 = 0;
    char title_id[14]{};
    char unknown2[0x3c]{};
};

std::vector<std::uint8_t> snapshot;
std::map<pid_t, MockAppInfo> app_infos;
int failures = 0;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL line %d: ", __LINE__); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
    } \
} while (0)

std::vector<std::uint8_t> process_record(pid_t pid, const char* command) {
    const std::size_t size = kCommOffset + std::strlen(command) + 1;
    std::vector<std::uint8_t> record(size, 0);
    const int record_size = static_cast<int>(size);
    std::memcpy(record.data(), &record_size, sizeof(record_size));
    std::memcpy(record.data() + kPidOffset, &pid, sizeof(pid));
    std::memcpy(record.data() + kCommOffset, command, std::strlen(command) + 1);
    return record;
}

void set_snapshot(std::initializer_list<std::vector<std::uint8_t>> records) {
    snapshot.clear();
    for (const auto& record : records)
        snapshot.insert(snapshot.end(), record.begin(), record.end());
}

void set_app_info(pid_t pid, std::uint32_t app_id, const char* title_id) {
    MockAppInfo info{};
    info.app_id = app_id;
    if (title_id)
        std::snprintf(info.title_id, sizeof(info.title_id), "%s", title_id);
    app_infos[pid] = info;
}

void reset_mocks() {
    snapshot.clear();
    app_infos.clear();
}

} // namespace

extern "C" long syscall(long number, ...) noexcept {
    if (number != kMockSysctlSyscall)
        return -1;

    va_list args;
    va_start(args, number);
    (void)va_arg(args, int*);       // mib
    (void)va_arg(args, int);        // mib length
    void* old_buffer = va_arg(args, void*);
    auto* old_length = va_arg(args, std::size_t*);
    (void)va_arg(args, void*);      // new data
    (void)va_arg(args, std::size_t);
    va_end(args);

    if (!old_length)
        return -1;
    if (!old_buffer) {
        *old_length = snapshot.size();
        return 0;
    }
    if (*old_length < snapshot.size()) {
        errno = ENOMEM;
        *old_length = snapshot.size();
        return -1;
    }
    std::memcpy(old_buffer, snapshot.data(), snapshot.size());
    *old_length = snapshot.size();
    return 0;
}

extern "C" int sceKernelGetAppInfo(pid_t pid, MockAppInfo* info) {
    const auto it = app_infos.find(pid);
    if (it == app_infos.end())
        return -1;
    *info = it->second;
    return 0;
}

namespace {
void test_bootstrap_eboot_candidate() {
    reset_mocks();
    set_snapshot({process_record(1001, "eboot.bin")});
    set_app_info(1001, 42, "PPSA00042");

    const auto result = prefbar::ps5::find_game_process();
    CHECK(result.size_query_rc == 0 && result.data_query_rc == 0,
          "valid process snapshot should be complete");
    CHECK(result.pid == 1001 && result.launch_identity.app_id == 42,
          "initial eboot should provide the application identity");
    CHECK(std::strcmp(result.launch_command, "eboot.bin") == 0,
          "bootstrap executable command should be recorded");
    CHECK(std::strcmp(result.launch_identity.title_id, "PPSA00042") == 0,
          "title ID should be copied from sceKernelGetAppInfo");
}

void test_secondary_executable_is_in_same_application() {
    reset_mocks();
    set_snapshot({
        process_record(1002, "multiplayer.elf"),
        process_record(1001, "eboot.bin")
    });
    set_app_info(1001, 42, "PPSA00042");
    set_app_info(1002, 42, "PPSA00042");
    const auto identity = prefbar::ps5::GameApplicationIdentity{42, "PPSA00042"};

    const auto result = prefbar::ps5::find_game_process(&identity, 1001);
    CHECK(result.data_query_rc == 0,
          "secondary executable snapshot should be complete");
    CHECK(result.tracked_process_count == 2,
          "both eboot and differently named executable should match one app");
    CHECK(result.tracked_pid == 1001,
          "existing tracked PID should be preferred while it remains present");
    CHECK(std::strcmp(result.tracked_command, "eboot.bin") == 0,
          "selected process command should be recorded");
}

void test_secondary_executable_after_eboot_exits() {
    reset_mocks();
    set_snapshot({process_record(1002, "multiplayer.elf")});
    set_app_info(1002, 42, "PPSA00042");
    const auto identity = prefbar::ps5::GameApplicationIdentity{42, "PPSA00042"};

    const auto result = prefbar::ps5::find_game_process(&identity, 1001);
    CHECK(result.data_query_rc == 0,
          "secondary-only snapshot should be complete");
    CHECK(result.pid == -1,
          "a secondary executable should not be misreported as a new eboot");
    CHECK(result.tracked_process_count == 1 && result.tracked_pid == 1002,
          "secondary executable should keep the tracked app session alive");
    CHECK(std::strcmp(result.tracked_command, "multiplayer.elf") == 0,
          "secondary executable command should be recorded");
}

void test_title_id_fallback_when_app_id_is_unavailable() {
    reset_mocks();
    set_snapshot({process_record(2051, "secondary.elf")});
    set_app_info(2051, 0, "PPSA00042");
    const auto identity = prefbar::ps5::GameApplicationIdentity{0, "PPSA00042"};

    const auto result = prefbar::ps5::find_game_process(&identity, 1001);
    CHECK(result.data_query_rc == 0,
          "title-ID fallback snapshot should be complete");
    CHECK(result.tracked_process_count == 1 && result.tracked_pid == 2051,
          "matching title ID should group processes when app ID is unavailable");
}

void test_unrelated_application_is_not_included() {
    reset_mocks();
    set_snapshot({process_record(2001, "background.elf")});
    set_app_info(2001, 99, "PPSA00099");
    const auto identity = prefbar::ps5::GameApplicationIdentity{42, "PPSA00042"};

    const auto result = prefbar::ps5::find_game_process(&identity, 1001);
    CHECK(result.tracked_process_count == 0 && result.tracked_pid == -1,
          "different app ID must not keep the tracked game active");
}

void test_multiple_distinct_eboot_candidates_are_ambiguous() {
    reset_mocks();
    set_snapshot({
        process_record(3001, "eboot.bin"),
        process_record(3002, "eboot.bin")
    });
    set_app_info(3001, 42, "PPSA00042");
    set_app_info(3002, 99, "PPSA00099");

    const auto result = prefbar::ps5::find_game_process();
    CHECK(result.launch_ambiguous && result.pid == -1,
          "finder must not pick an arbitrary one of two different app sessions");
}

void test_reused_pid_with_new_app_identity_is_not_the_tracked_game() {
    reset_mocks();
    set_snapshot({process_record(1001, "eboot.bin")});
    set_app_info(1001, 99, "PPSA00099");
    const auto identity = prefbar::ps5::GameApplicationIdentity{42, "PPSA00042"};

    const auto result = prefbar::ps5::find_game_process(&identity, 1001);
    CHECK(result.tracked_process_count == 0,
          "PID reuse with another app ID must not preserve the old session");
    CHECK(result.pid == 1001 && result.launch_identity.app_id == 99,
          "the reused PID should be reported as a new application candidate");
}

void test_appinfo_failure_does_not_bootstrap_unverified_process() {
    reset_mocks();
    set_snapshot({process_record(3051, "eboot.bin")});
    /* No sceKernelGetAppInfo response for this PID (e.g. it exited mid-scan). */

    const auto result = prefbar::ps5::find_game_process();
    CHECK(result.data_query_rc == 0,
          "the process snapshot itself remains complete");
    CHECK(result.pid == -1 && !result.launch_identity.valid(),
          "a vanished/unverified eboot must not start a game session");
}

void test_zero_length_snapshot_is_not_treated_as_empty_process_list() {
    reset_mocks();
    set_snapshot({});
    const auto result = prefbar::ps5::find_game_process();
    CHECK(result.data_query_rc != 0 && result.saved_errno == EPROTO,
          "zero-byte KERN_PROC snapshot must be treated as incomplete");
}

void test_malformed_snapshot_is_not_complete() {
    reset_mocks();
    std::vector<std::uint8_t> malformed(32, 0);
    const int invalid_size = 32;
    std::memcpy(malformed.data(), &invalid_size, sizeof(invalid_size));
    set_snapshot({malformed});

    const auto result = prefbar::ps5::find_game_process();
    CHECK(result.data_query_rc != 0 && result.saved_errno == EPROTO,
          "malformed process records must not be treated as an empty complete list");
    CHECK(result.pid == -1 && result.tracked_process_count == 0,
          "partial records must not leak candidate or tracked processes");
}

} // namespace

int main() {
    test_bootstrap_eboot_candidate();
    test_secondary_executable_is_in_same_application();
    test_secondary_executable_after_eboot_exits();
    test_title_id_fallback_when_app_id_is_unavailable();
    test_unrelated_application_is_not_included();
    test_multiple_distinct_eboot_candidates_are_ambiguous();
    test_reused_pid_with_new_app_identity_is_not_the_tracked_game();
    test_appinfo_failure_does_not_bootstrap_unverified_process();
    test_zero_length_snapshot_is_not_treated_as_empty_process_list();
    test_malformed_snapshot_is_not_complete();
    std::printf("prefbar game-process selftest: %s (%d failures)\n",
                failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
