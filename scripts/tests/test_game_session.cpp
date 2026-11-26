/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Host regression tests for application-session tracking.
 */
#include "prefbar/game_session.hpp"

#include <cstdio>
#include <cstring>

using namespace prefbar::ps5;

namespace {
int failures = 0;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL line %d: ", __LINE__); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
    } \
} while (0)

GameApplicationIdentity app(std::uint32_t app_id, const char* title_id) {
    GameApplicationIdentity value{};
    value.app_id = app_id;
    if (title_id)
        std::snprintf(value.title_id, sizeof(value.title_id), "%s", title_id);
    return value;
}

GameProcessObservation launch(pid_t pid, GameApplicationIdentity identity) {
    GameProcessObservation value{};
    value.scan_complete = true;
    value.launch_pid = pid;
    value.launch_identity = identity;
    std::snprintf(value.launch_command, sizeof(value.launch_command), "eboot.bin");
    return value;
}

GameProcessObservation tracked(pid_t pid, unsigned count,
                               GameApplicationIdentity identity) {
    GameProcessObservation value{};
    value.scan_complete = true;
    value.tracked_pid = pid;
    value.tracked_process_count = count;
    value.tracked_identity = identity;
    std::snprintf(value.tracked_command, sizeof(value.tracked_command), "game.elf");
    return value;
}

GameSessionTracker start_session(GameApplicationIdentity identity,
                                 pid_t pid = 100) {
    GameSessionTracker tracker(3, 2'000'000);
    (void)tracker.update(launch(pid, identity), 0);
    (void)tracker.update(launch(pid, identity), 50'000);
    const auto started = tracker.update(launch(pid, identity), 100'000);
    CHECK(started.event == GameSessionEvent::Started,
          "three stable observations should start a session");
    CHECK(started.ready && started.generation == 1,
          "new session should be ready with generation 1");
    return tracker;
}

void test_ordinary_single_executable_game() {
    const auto identity = app(42, "PPSA00001");
    auto tracker = start_session(identity, 101);
    for (unsigned i = 0; i < 20; ++i) {
        const auto state = tracker.update(tracked(101, 1, identity),
                                          150'000 + i * 50'000);
        CHECK(state.ready, "ordinary game lost readiness at sample %u", i);
        CHECK(state.generation == 1,
              "ordinary game created an extra session at sample %u", i);
    }
}

void test_launcher_to_secondary_executable() {
    const auto identity = app(77, "PPSA11111");
    auto tracker = start_session(identity, 110);
    const auto first = tracker.update(tracked(220, 1, identity), 150'000);
    CHECK(first.event == GameSessionEvent::ProcessChanged,
          "replacement PID should be recorded as a process transition");
    CHECK(first.ready && first.generation == 1,
          "launcher-to-game transition must preserve readiness and generation");
    CHECK(first.active_pid == 220,
          "active PID should follow the replacement process");
    CHECK(std::strcmp(first.active_command, "game.elf") == 0,
          "active executable command should update with the process transition");
}

void test_parent_child_overlap() {
    const auto identity = app(88, "PPSA22222");
    auto tracker = start_session(identity, 301);
    auto state = tracker.update(tracked(301, 2, identity), 150'000);
    CHECK(state.event == GameSessionEvent::ProcessesChanged,
          "process-set growth should be visible in diagnostics");
    CHECK(state.ready && state.process_count == 2,
          "both parent and child should be counted in one application");
    state = tracker.update(tracked(302, 1, identity), 200'000);
    CHECK(state.ready && state.process_count == 1 && state.active_pid == 302,
          "session should continue after the parent process exits");
    CHECK(state.generation == 1,
          "overlapping process replacement must not create a new session");
}

void test_multiplayer_executable_transition() {
    const auto identity = app(99, "PPSA33333");
    auto tracker = start_session(identity, 410);
    /* Secondary multiplayer executable has a different PID/name; the process
       scanner reports it by application identity, without requiring eboot.bin. */
    for (int i = 0; i < 8; ++i) {
        const auto state = tracker.update(tracked(500, 1, identity),
                                          150'000 + i * 50'000);
        CHECK(state.ready && state.generation == 1,
              "multiplayer transition interrupted telemetry at sample %d", i);
    }
}

void test_replacement_candidate_near_exit_grace_preserves_handover() {
    const auto first_app = app(91, "PPSA90001");
    const auto second_app = app(92, "PPSA90002");
    auto tracker = start_session(first_app, 905);

    GameProcessObservation empty{};
    empty.scan_complete = true;
    auto state = tracker.update(empty, 200'000);
    CHECK(state.ready, "old app should remain ready while its process is absent");
    state = tracker.update(empty, 1'900'000);
    CHECK(state.ready, "old app should remain ready just before exit grace");

    /* The replacement appears with only 100 ms left on the old grace timer. */
    state = tracker.update(launch(906, second_app), 2'100'000);
    CHECK(state.ready && state.generation == 1,
          "candidate verification must pause old-app expiry");
    state = tracker.update(launch(906, second_app), 2'350'000);
    CHECK(state.ready && state.generation == 1,
          "candidate should remain pending during its second stable sample");
    state = tracker.update(launch(906, second_app), 2'600'000);
    CHECK(state.event == GameSessionEvent::Switched && state.ready &&
          state.generation == 2,
          "verified replacement should switch without a readiness gap");
}

void test_real_game_exit() {
    const auto identity = app(123, "PPSA44444");
    auto tracker = start_session(identity, 601);
    GameProcessObservation empty{};
    empty.scan_complete = true;
    auto state = tracker.update(empty, 150'000);
    CHECK(state.ready, "session should survive the initial process-list gap");
    state = tracker.update(empty, 2'149'999);
    CHECK(state.ready, "session should remain ready before the 2-second grace expires");
    state = tracker.update(empty, 2'150'000);
    CHECK(state.event == GameSessionEvent::Ended && !state.ready,
          "session should end after 2 seconds of complete empty snapshots");
    CHECK(state.generation == 1, "ending must not increment generation");
    state = tracker.update(empty, 2'300'000);
    CHECK(state.event == GameSessionEvent::None && !state.ready,
          "genuine exit should be reported exactly once");
}

void test_unrelated_background_application() {
    const auto identity = app(150, "PPSA55555");
    const auto unrelated = app(151, "PPSA66666");
    auto tracker = start_session(identity, 701);
    for (int i = 0; i < 10; ++i) {
        auto state = tracked(701, 1, identity);
        state.launch_pid = 702;
        state.launch_identity = unrelated;
        const auto updated = tracker.update(state, 150'000 + i * 50'000);
        CHECK(updated.ready && updated.generation == 1,
              "unrelated eboot candidate stole an active app session");
        CHECK(same_game_application(updated.identity, identity),
              "active application identity changed due to unrelated process");
    }
}

void test_pid_reuse_does_not_preserve_wrong_application() {
    const auto first_app = app(170, "PPSA77777");
    const auto reused_pid_app = app(171, "PPSA88888");
    auto tracker = start_session(first_app, 801);
    auto state = tracker.update(launch(801, reused_pid_app), 150'000);
    CHECK(state.ready && state.generation == 1,
          "a single reused-PID observation must not immediately replace the session");
    state = tracker.update(launch(801, reused_pid_app), 200'000);
    state = tracker.update(launch(801, reused_pid_app), 250'000);
    CHECK(state.event == GameSessionEvent::Switched && state.ready,
          "stable different application identity should replace a reused PID");
    CHECK(state.generation == 2 &&
          same_game_application(state.identity, reused_pid_app),
          "PID reuse must not be mistaken for the original app");
}

void test_incomplete_snapshot_pauses_exit_timer() {
    const auto identity = app(190, "PPSA99991");
    auto tracker = start_session(identity, 901);
    GameProcessObservation failed{};
    failed.scan_complete = false;
    auto state = tracker.update(failed, 10'000'000);
    CHECK(state.event == GameSessionEvent::ScanFailed && state.ready,
          "failed scan should be reported without ending the session");
    GameProcessObservation empty{};
    empty.scan_complete = true;
    state = tracker.update(empty, 10'050'000);
    CHECK(state.ready, "first complete missing scan should start, not finish, grace");
    state = tracker.update(empty, 12'049'999);
    CHECK(state.ready, "exit timer should be based on complete missing scans");
    state = tracker.update(empty, 12'050'000);
    CHECK(state.event == GameSessionEvent::Ended,
          "session should end after a full grace period following a complete scan");
}

void test_multiple_matching_processes() {
    const auto identity = app(210, "PPSA12345");
    auto tracker = start_session(identity, 1001);
    auto state = tracker.update(tracked(1002, 3, identity), 150'000);
    CHECK(state.ready && state.process_count == 3,
          "all matching application processes should be retained");
    state = tracker.update(tracked(1003, 2, identity), 200'000);
    CHECK(state.ready && state.process_count == 2 && state.generation == 1,
          "process-count changes must not create application sessions");
}

void test_app_id_distinguishes_new_running_instance() {
    const auto old_identity = app(301, "PPSA54321");
    const auto new_identity = app(302, "PPSA54321");
    auto tracker = start_session(old_identity, 1101);
    CHECK(!same_game_application(old_identity, new_identity),
          "different live app IDs must not be merged solely by title ID");
    (void)tracker.update(launch(1102, new_identity), 150'000);
    (void)tracker.update(launch(1102, new_identity), 200'000);
    const auto state = tracker.update(launch(1102, new_identity), 250'000);
    CHECK(state.event == GameSessionEvent::Switched && state.ready &&
          state.generation == 2,
          "a new app ID for the same title should start a distinct session");
}

void test_app_id_groups_processes_even_if_title_metadata_is_incomplete() {
    const auto first = app(401, "PPSA11112");
    const auto secondary = app(401, "PPSAOTHER");
    CHECK(same_game_application(first, secondary),
          "matching live app IDs should associate related app processes");
}

void test_ambiguous_launch_candidates_are_not_guessed() {
    GameSessionTracker tracker(3, 2'000'000);
    auto observation = launch(1201, app(501, "PPSA12121"));
    observation.launch_ambiguous = true;
    auto state = tracker.update(observation, 0);
    CHECK(state.event == GameSessionEvent::CandidateAmbiguous && !state.ready,
          "ambiguous eboot candidates must not start an arbitrary session");
    state = tracker.update(observation, 50'000);
    CHECK(!state.ready && state.generation == 0,
          "ambiguity must not create a session generation");
}

} // namespace

int main() {
    test_ordinary_single_executable_game();
    test_launcher_to_secondary_executable();
    test_parent_child_overlap();
    test_multiplayer_executable_transition();
    test_replacement_candidate_near_exit_grace_preserves_handover();
    test_real_game_exit();
    test_unrelated_background_application();
    test_pid_reuse_does_not_preserve_wrong_application();
    test_incomplete_snapshot_pauses_exit_timer();
    test_multiple_matching_processes();
    test_app_id_distinguishes_new_running_instance();
    test_app_id_groups_processes_even_if_title_metadata_is_incomplete();
    test_ambiguous_launch_candidates_are_not_guessed();
    std::printf("prefbar game-session selftest: %s (%d failures)\n",
                failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
