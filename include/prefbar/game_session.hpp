/*
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Application-session tracking, deliberately independent of PS5 syscalls so
 * its transition and exit behaviour can be exercised by host tests.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <sys/types.h>

namespace prefbar::ps5 {

struct GameApplicationIdentity {
    std::uint32_t app_id = 0;
    char title_id[16]{};

    [[nodiscard]] bool valid() const noexcept {
        return app_id != 0 || title_id[0] != '\0';
    }
};

/*
 * app_id is the live big-app process-group identity and is preferred when
 * available. title_id is the fallback while app_id metadata is unavailable.
 * etaHEN uses app_info.app_id to associate processes with its running big app.
 */
[[nodiscard]] inline bool same_game_application(
    const GameApplicationIdentity& a,
    const GameApplicationIdentity& b) noexcept {
    const bool a_has_title = a.title_id[0] != '\0';
    const bool b_has_title = b.title_id[0] != '\0';
    if (a.app_id != 0 && b.app_id != 0)
        return a.app_id == b.app_id;
    return a_has_title && b_has_title &&
        std::strncmp(a.title_id, b.title_id, sizeof(a.title_id)) == 0;
}

struct GameProcessObservation {
    bool scan_complete = false;

    /* An eboot.bin process can bootstrap a new game session. */
    pid_t launch_pid = -1;
    GameApplicationIdentity launch_identity{};
    char launch_command[24]{};
    bool launch_ambiguous = false;

    /* All processes matching the active application's identity. */
    pid_t tracked_pid = -1;
    unsigned tracked_process_count = 0;
    GameApplicationIdentity tracked_identity{};
    char tracked_command[24]{};
};

enum class GameSessionEvent {
    None,
    ScanFailed,
    CandidateAmbiguous,
    Started,
    ProcessChanged,
    ProcessesChanged,
    Switched,
    Ended,
};

struct GameSessionUpdate {
    GameSessionEvent event = GameSessionEvent::None;
    bool ready = false;
    pid_t active_pid = -1;
    unsigned process_count = 0;
    std::uint64_t generation = 0;
    GameApplicationIdentity identity{};
    char active_command[24]{};
    pid_t previous_pid = -1;
    GameApplicationIdentity previous_identity{};
};

class GameSessionTracker {
public:
    explicit GameSessionTracker(
        unsigned stable_samples = 3,
        std::uint64_t exit_grace_us = 2'000'000ULL) noexcept
        : stable_samples_(stable_samples ? stable_samples : 1),
          exit_grace_us_(exit_grace_us) {}

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] pid_t active_pid() const noexcept { return active_pid_; }
    [[nodiscard]] const GameApplicationIdentity& identity() const noexcept {
        return active_identity_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] GameSessionUpdate update(
        const GameProcessObservation& observation,
        std::uint64_t now_us) noexcept {
        GameSessionEvent event = GameSessionEvent::None;
        pid_t previous_pid = -1;
        GameApplicationIdentity previous_identity{};

        if (!observation.scan_complete) {
            missing_active_ = false;
            reset_candidate();
            if (!scan_failure_reported_) {
                scan_failure_reported_ = true;
                event = GameSessionEvent::ScanFailed;
            }
            return make_update(event, previous_pid, previous_identity);
        }
        scan_failure_reported_ = false;

        if (!active_) {
            if (observation.launch_ambiguous) {
                reset_candidate();
                return make_update(GameSessionEvent::CandidateAmbiguous,
                                   previous_pid, previous_identity);
            }
            if (observation.launch_pid <= 0) {
                reset_candidate();
                return make_update(event, previous_pid, previous_identity);
            }
            if (observe_candidate(observation.launch_pid,
                                  observation.launch_identity,
                                  observation.launch_command)) {
                start_candidate();
                event = GameSessionEvent::Started;
            }
            return make_update(event, previous_pid, previous_identity);
        }

        if (observation.tracked_process_count > 0) {
            missing_active_ = false;
            const bool process_count_changed =
                process_count_ != observation.tracked_process_count;
            process_count_ = observation.tracked_process_count;

            if (!active_identity_.valid() && observation.tracked_identity.valid())
                active_identity_ = observation.tracked_identity;

            if (observation.tracked_pid > 0 &&
                observation.tracked_pid != active_pid_) {
                previous_pid = active_pid_;
                previous_identity = active_identity_;
                active_pid_ = observation.tracked_pid;
                event = GameSessionEvent::ProcessChanged;
            } else if (process_count_changed) {
                event = GameSessionEvent::ProcessesChanged;
            }
            if (observation.tracked_command[0] != '\0')
                copy_command(active_command_, observation.tracked_command);

            /* A second application's eboot must not steal an extant session. */
            reset_candidate();
            return make_update(event, previous_pid, previous_identity);
        }

        process_count_ = 0;
        if (observation.launch_ambiguous) {
            reset_candidate();
            event = GameSessionEvent::CandidateAmbiguous;
        } else if (is_different_application_candidate(observation)) {
            if (observe_candidate(observation.launch_pid,
                                  observation.launch_identity,
                                  observation.launch_command)) {
                previous_pid = active_pid_;
                previous_identity = active_identity_;
                start_candidate();
                event = GameSessionEvent::Switched;
                return make_update(event, previous_pid, previous_identity);
            }
            /* A replacement app is being verified; don't expire the old
               session before the candidate has had its stability samples. */
            missing_active_ = false;
            return make_update(event, previous_pid, previous_identity);
        } else {
            reset_candidate();
        }

        /*
         * Only a complete process snapshot can start/advance the exit timer.
         * A failed scan pauses it, and an executable handover keeps readiness
         * asserted while the replacement candidate stabilizes.
         */
        if (!missing_active_) {
            missing_active_ = true;
            missing_since_us_ = now_us;
        }
        if (now_us >= missing_since_us_ &&
            now_us - missing_since_us_ >= exit_grace_us_) {
            previous_pid = active_pid_;
            previous_identity = active_identity_;
            active_ = false;
            active_pid_ = -1;
            process_count_ = 0;
            active_identity_ = {};
            active_command_[0] = '\0';
            missing_active_ = false;
            reset_candidate();
            event = GameSessionEvent::Ended;
        }
        return make_update(event, previous_pid, previous_identity);
    }

private:
    static void copy_command(char (&out)[24], const char* in) noexcept {
        std::size_t length = 0;
        if (in) {
            while (length < sizeof(out) - 1 && in[length] != '\0')
                ++length;
            std::memcpy(out, in, length);
        }
        out[length] = '\0';
    }

    [[nodiscard]] bool candidate_matches(
        pid_t pid,
        const GameApplicationIdentity& identity) const noexcept {
        if (candidate_identity_.valid() && identity.valid())
            return same_game_application(candidate_identity_, identity);
        return candidate_pid_ > 0 && candidate_pid_ == pid;
    }

    bool observe_candidate(
        pid_t pid,
        const GameApplicationIdentity& identity,
        const char* command) noexcept {
        if (candidate_samples_ == 0 || !candidate_matches(pid, identity)) {
            candidate_pid_ = pid;
            candidate_identity_ = identity;
            copy_command(candidate_command_, command);
            candidate_samples_ = 1;
        } else {
            candidate_pid_ = pid;
            if (identity.valid())
                candidate_identity_ = identity;
            if (command && command[0] != '\0')
                copy_command(candidate_command_, command);
            if (candidate_samples_ < stable_samples_)
                ++candidate_samples_;
        }
        return candidate_samples_ >= stable_samples_;
    }

    [[nodiscard]] bool is_different_application_candidate(
        const GameProcessObservation& observation) const noexcept {
        if (observation.launch_pid <= 0)
            return false;
        if (active_identity_.valid()) {
            return observation.launch_identity.valid() &&
                !same_game_application(active_identity_, observation.launch_identity);
        }
        return observation.launch_pid != active_pid_;
    }

    void start_candidate() noexcept {
        active_ = true;
        active_pid_ = candidate_pid_;
        active_identity_ = candidate_identity_;
        copy_command(active_command_, candidate_command_);
        process_count_ = 1;
        missing_active_ = false;
        ++generation_;
        reset_candidate();
    }

    void reset_candidate() noexcept {
        candidate_pid_ = -1;
        candidate_identity_ = {};
        candidate_command_[0] = '\0';
        candidate_samples_ = 0;
    }

    [[nodiscard]] GameSessionUpdate make_update(
        GameSessionEvent event,
        pid_t previous_pid,
        const GameApplicationIdentity& previous_identity) const noexcept {
        GameSessionUpdate result{};
        result.event = event;
        result.ready = active_;
        result.active_pid = active_pid_;
        result.process_count = process_count_;
        result.generation = generation_;
        result.identity = active_identity_;
        copy_command(result.active_command, active_command_);
        result.previous_pid = previous_pid;
        result.previous_identity = previous_identity;
        return result;
    }

    const unsigned stable_samples_;
    const std::uint64_t exit_grace_us_;
    bool active_ = false;
    bool scan_failure_reported_ = false;
    bool missing_active_ = false;
    std::uint64_t missing_since_us_ = 0;
    std::uint64_t generation_ = 0;
    pid_t active_pid_ = -1;
    unsigned process_count_ = 0;
    GameApplicationIdentity active_identity_{};
    char active_command_[24]{};
    pid_t candidate_pid_ = -1;
    unsigned candidate_samples_ = 0;
    GameApplicationIdentity candidate_identity_{};
    char candidate_command_[24]{};
};

} // namespace prefbar::ps5
