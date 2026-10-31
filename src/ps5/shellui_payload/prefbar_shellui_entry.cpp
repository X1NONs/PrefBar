/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 * Modifications Copyright (C) 2026 erickdavestech
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefbar_shellui.hpp"
#include "prefbar/shellui_stage.hpp"

#include <cstdio>
#include <unistd.h>

namespace {

constexpr const char* kMarker = "/system_tmp/prefbar_shellui.pid";
constexpr const char* kLog =
    "/system_tmp/prefbar_shellui.log";

void write_online_marker() {
    FILE* fp = std::fopen(kMarker, "w");
    if (!fp)
        return;
    std::fprintf(fp, "%d\n", getpid());
    std::fclose(fp);
}

[[noreturn]] void park_renderer_failure(const char* stage) {
    if (FILE* fp = std::fopen(kLog, "a")) {
        std::fprintf(
            fp,
            "renderer parked stage=%s injected_thread_return=disabled\n",
            stage);
        std::fclose(fp);
    }

    /*
     * Returning an injected ShellUI ELF thread can destabilize the console.
     * A compatibility or socket failure therefore remains inert and resident
     * instead of returning through the loader. The V0.2.0 renderer preserves that proven
     * lifecycle and patches a native hook only while ShellUI is stopped.
     */
    for (;;)
        usleep(1000000);
}

} // namespace

namespace prefbar::ps5::shellui {

void record_stage(const char* stage) noexcept {
    if (FILE* fp = std::fopen(kShellUiStagePath, "w")) {
        std::fprintf(fp, "%d %.47s\n", getpid(), stage);
        std::fclose(fp);
    }
}

} // namespace prefbar::ps5::shellui

extern "C" __attribute__((noreturn, visibility("default")))
void* elf_main(void* payload_args) {
    using namespace prefbar::ps5::shellui;

    /* A shared renderer does not start the PS5 payload CRT in SceShellUI. */
    (void)payload_args;

    record_stage("entry");

    if (FILE* fp = std::fopen(kLog, "w")) {
        std::fputs(
            "PrefBar V0.2.0 renderer\n",
            fp);
        std::fclose(fp);
    }

    bool runtime_ready = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (initialize_runtime()) {
            runtime_ready = true;
            break;
        }
        usleep(1000000);
    }

    if (!runtime_ready)
        park_renderer_failure("runtime");

    record_stage("runtime_ready");

    if (!initialize_receiver())
        park_renderer_failure("receiver");

    record_stage("receiver_ready");

    write_online_marker();
    run_receiver_loop();

    __builtin_unreachable();
}
