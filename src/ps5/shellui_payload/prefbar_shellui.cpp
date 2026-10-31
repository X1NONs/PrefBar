/*
 * PrefBar PS5 component (derived-source notices in CREDITS.md)
 * Copyright (C) 2026 porhe911
 * Modifications Copyright (C) 2026 khalifa007 (SimpleFPS)
 * Modifications Copyright (C) 2026 erickdavestech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefbar_shellui.hpp"
#include "prefbar/shellui_hook_protocol.hpp"
#include "prefbar/telemetry_wire.h"
#include "prefbar/touchpad_toggle.hpp"

extern "C" {
int scePadOpen(int user_id, int type, int index, const void* param);
int scePadClose(int handle);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, void* data);
int sceUserServiceGetForegroundUser(int* user_id);
}

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace prefbar::ps5::shellui {

namespace {

static_assert(SOL_SOCKET == 0xffff);
static_assert(SO_RCVTIMEO == 0x1006);
static_assert(sizeof(timeval) == 16);

struct MonoDomain;
struct MonoThread;
struct MonoAssembly;
struct MonoImage;
struct MonoClass;
struct MonoMethod;
struct MonoProperty;
struct MonoObject;
struct MonoString;

using mono_get_root_domain_t = MonoDomain* (*)();
using mono_domain_get_t = MonoDomain* (*)();
using mono_thread_attach_t = MonoThread* (*)(MonoDomain*);
using mono_domain_assembly_open_t = MonoAssembly* (*)(MonoDomain*, const char*);
using mono_assembly_get_image_t = MonoImage* (*)(MonoAssembly*);
using mono_class_from_name_t = MonoClass* (*)(MonoImage*, const char*, const char*);
using mono_class_get_method_from_name_t = MonoMethod* (*)(MonoClass*, const char*, int);
using mono_class_get_property_from_name_t = MonoProperty* (*)(MonoClass*, const char*);
using mono_property_get_get_method_t = MonoMethod* (*)(MonoProperty*);
using mono_property_get_set_method_t = MonoMethod* (*)(MonoProperty*);
using mono_runtime_invoke_t = MonoObject* (*)(MonoMethod*, void*, void**, MonoObject**);
using mono_string_new_t = MonoString* (*)(MonoDomain*, const char*);
using mono_object_new_t = MonoObject* (*)(MonoDomain*, MonoClass*);
using mono_runtime_object_init_t = void (*)(MonoObject*);
using mono_object_unbox_t = void* (*)(MonoObject*);
using mono_compile_method_t = void* (*)(MonoMethod*);
using mono_gchandle_new_t = std::uint32_t (*)(MonoObject*, int);
using mono_gchandle_get_target_t = MonoObject* (*)(std::uint32_t);
using mono_gchandle_free_t = void (*)(std::uint32_t);

extern "C" {
MonoDomain* mono_get_root_domain();
MonoDomain* mono_domain_get();
MonoThread* mono_thread_attach(MonoDomain*);
MonoAssembly* mono_domain_assembly_open(MonoDomain*, const char*);
MonoImage* mono_assembly_get_image(MonoAssembly*);
MonoClass* mono_class_from_name(MonoImage*, const char*, const char*);
MonoMethod* mono_class_get_method_from_name(MonoClass*, const char*, int);
MonoProperty* mono_class_get_property_from_name(MonoClass*, const char*);
MonoMethod* mono_property_get_get_method(MonoProperty*);
MonoMethod* mono_property_get_set_method(MonoProperty*);
MonoObject* mono_runtime_invoke(MonoMethod*, void*, void**, MonoObject**);
MonoString* mono_string_new(MonoDomain*, const char*);
MonoObject* mono_object_new(MonoDomain*, MonoClass*);
void mono_runtime_object_init(MonoObject*);
void* mono_object_unbox(MonoObject*);
void* mono_compile_method(MonoMethod*);
std::uint32_t mono_gchandle_new(MonoObject*, int);
MonoObject* mono_gchandle_get_target(std::uint32_t);
void mono_gchandle_free(std::uint32_t);
}

mono_get_root_domain_t mono_get_root_domain_ = mono_get_root_domain;
mono_domain_get_t mono_domain_get_ = mono_domain_get;
mono_thread_attach_t mono_thread_attach_ = mono_thread_attach;
mono_domain_assembly_open_t mono_domain_assembly_open_ =
    mono_domain_assembly_open;
mono_assembly_get_image_t mono_assembly_get_image_ = mono_assembly_get_image;
mono_class_from_name_t mono_class_from_name_ = mono_class_from_name;
mono_class_get_method_from_name_t mono_class_get_method_from_name_ =
    mono_class_get_method_from_name;
mono_class_get_property_from_name_t mono_class_get_property_from_name_ =
    mono_class_get_property_from_name;
mono_property_get_get_method_t mono_property_get_get_method_ =
    mono_property_get_get_method;
mono_property_get_set_method_t mono_property_get_set_method_ =
    mono_property_get_set_method;
mono_runtime_invoke_t mono_runtime_invoke_ = mono_runtime_invoke;
mono_string_new_t mono_string_new_ = mono_string_new;
mono_object_new_t mono_object_new_ = mono_object_new;
mono_runtime_object_init_t mono_runtime_object_init_ =
    mono_runtime_object_init;
mono_object_unbox_t mono_object_unbox_ = mono_object_unbox;
mono_compile_method_t mono_compile_method_ = mono_compile_method;
mono_gchandle_new_t mono_gchandle_new_ = mono_gchandle_new;
mono_gchandle_get_target_t mono_gchandle_get_target_ =
    mono_gchandle_get_target;
mono_gchandle_free_t mono_gchandle_free_ = mono_gchandle_free;

MonoDomain* g_domain{};
MonoImage* g_pui_image{};
std::uint32_t g_game_scene_handle{};

int g_pad_handle = -1;
int g_pad_init_state = 0;
int g_pad_user = -1;
MonoMethod* g_find_scene_method{};
bool g_background_render_mode = false;


using application_update_t = void (*)(MonoObject*);
application_update_t g_application_update_original{};
bool g_hook_install_confirmed = false;
bool g_hook_attempted = false;

std::atomic_bool g_runtime_ready{false};

int g_receiver_fd = -1;

constexpr const char* kLabelId = "id_prefbar_label";
constexpr const char* kValueId = "id_prefbar_value";

/* ---- prefbar-overlay: text rows fed over loopback UDP by the separate
 * prefbar payload. State is written by the receiver thread under g_telemetry_mutex
 * and drawn from the update hook (same thread as the rest of the drawing). */
constexpr std::uint64_t kTelemetryStaleMs = 3000;      /* rows go stale if prefbar goes quiet */
constexpr bool kShowTelemetryStatus = true;         /* banner when there is no fresh data (test builds) */
bool g_prefbar_overlay_enabled = true;             /* touchpad toggle; sampling stays active */
std::atomic<bool> g_config_overlay_enabled{true};
std::atomic<bool> g_touchpad_toggle_enabled{true};
prefbar::TouchpadTripleClick g_touchpad_triple_click{};
prefbar_wire_t g_telemetry_packet{};
pthread_mutex_t g_telemetry_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<std::uint64_t> g_telemetry_rx_count{0};
std::atomic<std::uint64_t> g_telemetry_rx_ms{0};
std::uint64_t g_telemetry_applied_count = 0;
bool g_telemetry_hidden = false;
std::uint32_t g_telemetry_row_handle[PREFBAR_ROWS]{};
std::uint32_t g_telemetry_shadow_handle[PREFBAR_ROWS]{};
struct TelemetryLayout {
    int font;
    int corner;
    int rows;
    float mx;
    float my;
    float r;
    float g;
    float b;
    bool shadow;
    bool valid;
};
TelemetryLayout g_telemetry_layout{};
std::atomic<bool> g_telemetry_draw_busy{false};   /* the draw routine must never run twice at once */
int g_row_rebuild_count = 0;

struct TelemetryDrawGuard {
    explicit TelemetryDrawGuard(std::atomic<bool>& f) : flag(f), owns(!f.exchange(true)) {}
    ~TelemetryDrawGuard() { if (owns) flag.store(false); }
    std::atomic<bool>& flag;
    bool owns;
};
constexpr const char* kPuiDll =
    "/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll";
constexpr const char* kAppSystemDll =
    "/system_ex/common_ex/lib/Sce.Vsh.ShellUI.AppSystem.dll";
constexpr const char* kCoreDll =
    "/system_ex/common_ex/lib/Sce.PlayStation.Core.dll";

extern "C" __attribute__((naked, noinline, used, aligned(16)))
void prefbar_update_trampoline() {
    __asm__ volatile(
        ".rept 128\n"
        "nop\n"
        ".endr\n"
        "ret\n");
}

void application_update_hook(MonoObject* instance);

constexpr std::size_t kAbsoluteJumpSize = 14;
constexpr std::size_t kAtomicPatchSize = 16;
constexpr std::size_t kTrampolineCapacity = 128;

void encode_absolute_jump(
    std::uint8_t* output,
    const void* destination) noexcept {

    static constexpr std::uint8_t kPrefix[6] = {
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
    };
    std::memcpy(output, kPrefix, sizeof(kPrefix));
    const std::uint64_t target =
        reinterpret_cast<std::uint64_t>(destination);
    std::memcpy(output + sizeof(kPrefix), &target, sizeof(target));
}

bool prepare_trampoline(
    const std::uint8_t* original,
    std::size_t original_size,
    const void* continuation) noexcept {

    if (!original || original_size == 0 ||
        original_size + (continuation ? kAbsoluteJumpSize : 0) >
            kTrampolineCapacity) {
        return false;
    }

    auto* trampoline = reinterpret_cast<std::uint8_t*>(
        &prefbar_update_trampoline);
    std::memcpy(trampoline, original, original_size);
    std::size_t written = original_size;
    if (continuation) {
        encode_absolute_jump(trampoline + written, continuation);
        written += kAbsoluteJumpSize;
    }

    __builtin___clear_cache(
        reinterpret_cast<char*>(trampoline),
        reinterpret_cast<char*>(trampoline + written));
    return true;
}

std::size_t decode_relocatable_instruction(
    const std::uint8_t* code,
    std::size_t available) noexcept {

    if (!code || available == 0)
        return 0;

    std::size_t cursor = 0;
    bool rex_w = false;
    bool operand16 = false;

    while (cursor < available) {
        const std::uint8_t byte = code[cursor];
        if (byte == 0x66) {
            operand16 = true;
            ++cursor;
            continue;
        }
        if (byte == 0x67) {

            return 0;
        }
        if (byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
            byte == 0x2e || byte == 0x36 || byte == 0x3e ||
            byte == 0x26 || byte == 0x64 || byte == 0x65) {
            ++cursor;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex_w = (byte & 0x08U) != 0;
            ++cursor;
            continue;
        }
        break;
    }

    if (cursor >= available)
        return 0;

    const std::uint8_t opcode = code[cursor++];

    if ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x90)
        return cursor;

    if (opcode == 0x68) {
        const std::size_t immediate = operand16 ? 2U : 4U;
        return cursor + immediate <= available
            ? cursor + immediate
            : 0;
    }
    if (opcode == 0x6a)
        return cursor + 1U <= available ? cursor + 1U : 0;

    if (opcode >= 0xb8 && opcode <= 0xbf) {
        const std::size_t immediate = rex_w ? 8U : (operand16 ? 2U : 4U);
        return cursor + immediate <= available
            ? cursor + immediate
            : 0;
    }

    if (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb ||
        (opcode >= 0x70 && opcode <= 0x7f) ||
        (opcode >= 0xe0 && opcode <= 0xe3)) {
        return 0;
    }

    std::size_t immediate = 0;
    bool has_modrm = false;

    switch (opcode) {
    case 0x01: case 0x03: case 0x09: case 0x0b:
    case 0x21: case 0x23: case 0x29: case 0x2b:
    case 0x31: case 0x33: case 0x39: case 0x3b:
    case 0x63: case 0x85: case 0x87: case 0x89:
    case 0x8b: case 0x8d:
        has_modrm = true;
        break;
    case 0x80: case 0x82: case 0x83: case 0xc6:
        has_modrm = true;
        immediate = 1;
        break;
    case 0x81: case 0xc7:
        has_modrm = true;
        immediate = operand16 ? 2U : 4U;
        break;
    case 0x0f: {
        if (cursor >= available)
            return 0;
        const std::uint8_t second = code[cursor++];
        if (second >= 0x80 && second <= 0x8f)
            return 0;
        if (second == 0x1e || second == 0x1f ||
            second == 0xb6 || second == 0xb7 ||
            second == 0xbe || second == 0xbf) {
            has_modrm = true;
            break;
        }
        return 0;
    }
    default:
        return 0;
    }

    if (!has_modrm || cursor >= available)
        return 0;

    const std::uint8_t modrm = code[cursor++];
    const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6);
    const std::uint8_t rm = static_cast<std::uint8_t>(modrm & 7U);

    if (mod != 3 && rm == 4) {
        if (cursor >= available)
            return 0;
        const std::uint8_t sib = code[cursor++];
        const std::uint8_t base = static_cast<std::uint8_t>(sib & 7U);
        if (mod == 0 && base == 5) {
            if (cursor + 4U > available)
                return 0;
            cursor += 4U;
        }
    } else if (mod == 0 && rm == 5) {

        return 0;
    }

    if (mod == 1) {
        if (cursor + 1U > available)
            return 0;
        cursor += 1U;
    } else if (mod == 2) {
        if (cursor + 4U > available)
            return 0;
        cursor += 4U;
    }

    if (cursor + immediate > available)
        return 0;
    return cursor + immediate;
}

std::size_t native_patch_length(
    const std::uint8_t* code) noexcept {

    std::size_t length = 0;
    while (length < kAbsoluteJumpSize) {
        const std::size_t decoded = decode_relocatable_instruction(
            code + length,
            kAtomicPatchSize - length);
        if (decoded == 0)
            return 0;
        length += decoded;
    }

    return length <= kAtomicPatchSize ? length : 0;
}

template <typename T>
bool write_exact_file(
    const char* path,
    const T& value) noexcept {

    FILE* fp = std::fopen(path, "wb");
    if (!fp)
        return false;

    const bool complete =
        std::fwrite(&value, 1, sizeof(value), fp) == sizeof(value) &&
        std::fflush(fp) == 0;
    const bool closed = std::fclose(fp) == 0;
    return complete && closed;
}

template <typename T>
bool read_exact_file(const char* path, T& value) noexcept {
    FILE* fp = std::fopen(path, "rb");
    if (!fp)
        return false;

    const std::size_t count = std::fread(&value, 1, sizeof(value), fp);
    std::fclose(fp);
    return count == sizeof(value);
}

bool publish_hook_request(
    std::uint8_t* method,
    const std::array<std::uint8_t, kAtomicPatchSize>& expected,
    const std::array<std::uint8_t, kAtomicPatchSize>& desired,
    std::size_t displaced_size,
    application_update_t original_call,
    std::uint64_t& nonce) noexcept {

    ShellUiHookRequest request{};
    request.pid = getpid();
    request.method_address =
        reinterpret_cast<std::uint64_t>(method);
    request.hook_address =
        reinterpret_cast<std::uint64_t>(&application_update_hook);
    request.original_call_address =
        reinterpret_cast<std::uint64_t>(original_call);
    request.displaced_size = static_cast<std::uint32_t>(displaced_size);
    std::memcpy(request.expected, expected.data(), expected.size());
    std::memcpy(request.desired, desired.data(), desired.size());
    request.nonce = request.method_address ^ request.hook_address ^
        request.original_call_address ^
        (static_cast<std::uint64_t>(request.pid) << 32U) ^
        0x8d4f23a76c19e502ULL;
    if (request.nonce == 0)
        request.nonce = 1;
    request.checksum = shellui_hook_request_checksum(request);
    nonce = request.nonce;

    return write_exact_file(
        kShellUiHookRequestPath,
        request);
}

bool wait_for_native_hook_ack(
    pid_t pid,
    std::uint64_t nonce,
    ShellUiHookAck& ack) noexcept;

void log_line(const char* fmt, ...);
MonoImage* open_image(const char* path);

bool probe_hook_bytes(
    std::uint8_t* method,
    std::array<std::uint8_t, kAtomicPatchSize>& expected) noexcept {

    const std::array<std::uint8_t, kAtomicPatchSize> empty{};
    std::uint64_t nonce = 0;
    record_stage("hook_probe_request");
    if (!publish_hook_request(
            method,
            empty,
            empty,
            0,
            nullptr,
            nonce)) {
        log_line(
            "Application.Update probe publish failed method=%p",
            static_cast<void*>(method));
        return false;
    }

    ShellUiHookAck ack{};
    record_stage("hook_probe_ack_wait");
    if (!wait_for_native_hook_ack(getpid(), nonce, ack)) {
        log_line(
            "Application.Update probe timeout method=%p nonce=0x%llx",
            static_cast<void*>(method),
            static_cast<unsigned long long>(nonce));
        return false;
    }

    if (ack.phase != static_cast<std::uint8_t>(ShellUiHookAckPhase::Probe) ||
        ack.status != static_cast<std::int32_t>(ShellUiHookStatus::Success) ||
        ack.verified == 0 || ack.detached == 0 || ack.auth_restored == 0) {
        log_line(
            "Application.Update probe failed method=%p status=%d "
            "read_rc=%d verified=%u detached=%u auth_restored=%u phase=%u",
            static_cast<void*>(method),
            ack.status,
            ack.read_rc,
            static_cast<unsigned>(ack.verified),
            static_cast<unsigned>(ack.detached),
            static_cast<unsigned>(ack.auth_restored),
            static_cast<unsigned>(ack.phase));
        return false;
    }

    std::memcpy(expected.data(), ack.observed, expected.size());
    return true;
}

bool wait_for_native_hook_ack(
    pid_t pid,
    std::uint64_t nonce,
    ShellUiHookAck& ack) noexcept {

    for (int attempt = 0; attempt < 1000; ++attempt) {
        ShellUiHookAck candidate{};
        if (read_exact_file(kShellUiHookAckPath, candidate) &&
            candidate.magic == kShellUiHookAckMagic &&
            candidate.version == kShellUiHookProtocolVersion &&
            candidate.pid == pid &&
            candidate.nonce == nonce &&
            candidate.checksum == shellui_hook_ack_checksum(candidate)) {
            ack = candidate;
            return true;
        }
        usleep(20000);
    }
    return false;
}

enum class MainThreadGuardResult {
    Installed,
    Unsupported,
    Failed,
};

bool publish_main_thread_guard_request(
    std::uint8_t* method,
    const std::array<std::uint8_t, kAtomicPatchSize>& expected,
    std::uint64_t& nonce) noexcept {

    ShellUiHookRequest request{};
    request.pid = getpid();
    request.method_address = reinterpret_cast<std::uint64_t>(method);
    request.hook_address = request.method_address;
    request.original_call_address = 0;
    request.displaced_size = 1;
    std::memcpy(request.expected, expected.data(), expected.size());
    std::memcpy(request.desired, expected.data(), expected.size());
    request.desired[0] = 0xc3;
    request.nonce = request.method_address ^
        (static_cast<std::uint64_t>(request.pid) << 32U) ^
        0x6d9bc2f1a54837e0ULL;
    if (request.nonce == 0)
        request.nonce = 1;
    request.checksum = shellui_hook_request_checksum(request);
    nonce = request.nonce;
    return write_exact_file(kShellUiHookRequestPath, request);
}

MainThreadGuardResult install_main_thread_guard() {
    record_stage("legacy_guard_lookup");

    MonoImage* core = open_image(kCoreDll);
    if (!core) {
        log_line("legacy guard: core image unavailable");
        return MainThreadGuardResult::Failed;
    }

    MonoClass* diagnostics = mono_class_from_name_(
        core,
        "Sce.PlayStation.Core.Runtime",
        "Diagnostics");
    MonoMethod* check = diagnostics
        ? mono_class_get_method_from_name_(
              diagnostics,
              "CheckRunningOnMainThread",
              0)
        : nullptr;
    if (!check) {
        log_line("legacy guard: CheckRunningOnMainThread unavailable");
        return MainThreadGuardResult::Failed;
    }

    record_stage("legacy_guard_compile");
    auto* address = static_cast<std::uint8_t*>(mono_compile_method_(check));
    if (!address) {
        log_line("legacy guard: compile failed");
        return MainThreadGuardResult::Failed;
    }

    std::array<std::uint8_t, kAtomicPatchSize> expected{};
    record_stage("legacy_guard_probe");
    if (!probe_hook_bytes(address, expected)) {
        log_line("legacy guard: probe failed method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Failed;
    }

    if (expected[0] == 0xc3) {
        log_line("legacy guard already disabled method=%p",
                 static_cast<void*>(address));
        record_stage("legacy_guard_ready");
        return MainThreadGuardResult::Installed;
    }

    std::uint64_t nonce = 0;
    record_stage("legacy_guard_request");
    if (!publish_main_thread_guard_request(address, expected, nonce)) {
        log_line("legacy guard: request publish failed method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Failed;
    }

    ShellUiHookAck ack{};
    record_stage("legacy_guard_ack_wait");
    if (!wait_for_native_hook_ack(getpid(), nonce, ack)) {
        log_line("legacy guard: request timeout method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Failed;
    }

    if (ack.status ==
        static_cast<std::int32_t>(ShellUiHookStatus::UnsupportedFirmware)) {
        log_line("legacy guard unsupported by controller method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Unsupported;
    }

    if (ack.phase != static_cast<std::uint8_t>(
            ShellUiHookAckPhase::MainThreadGuard) ||
        ack.status != static_cast<std::int32_t>(ShellUiHookStatus::Success) ||
        ack.verified == 0 || ack.detached == 0 || ack.auth_restored == 0) {
        log_line(
            "legacy guard failed method=%p status=%d read_rc=%d write_rc=%d "
            "verified=%u restored=%u detached=%u auth_restored=%u phase=%u",
            static_cast<void*>(address),
            ack.status,
            ack.read_rc,
            ack.write_rc,
            static_cast<unsigned>(ack.verified),
            static_cast<unsigned>(ack.restored),
            static_cast<unsigned>(ack.detached),
            static_cast<unsigned>(ack.auth_restored),
            static_cast<unsigned>(ack.phase));
        return MainThreadGuardResult::Failed;
    }

    log_line(
        "legacy main-thread guard online method=%p stopped_patch=1 "
        "bytes_changed=1 verified=1",
        static_cast<void*>(address));
    record_stage("legacy_guard_ready");
    return MainThreadGuardResult::Installed;
}

void log_line(const char* fmt, ...) {
    FILE* fp = std::fopen(
        "/system_tmp/prefbar_shellui.log", "a");
    if (!fp)
        return;

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fputc('\n', fp);
    std::fclose(fp);
}

MonoImage* open_image(const char* path) {
    if (!g_domain || !mono_domain_assembly_open_ || !mono_assembly_get_image_)
        return nullptr;

    MonoAssembly* assembly = mono_domain_assembly_open_(g_domain, path);
    return assembly ? mono_assembly_get_image_(assembly) : nullptr;
}

MonoObject* invoke(
    MonoMethod* method,
    MonoObject* instance,
    void** args = nullptr) {

    if (!method || !mono_runtime_invoke_)
        return nullptr;

    MonoObject* exception = nullptr;
    MonoObject* result = mono_runtime_invoke_(
        method,
        instance,
        args,
        &exception);

    return exception ? nullptr : result;
}

MonoMethod* property_getter(MonoClass* klass, const char* name) {
    if (!klass)
        return nullptr;
    MonoProperty* property =
        mono_class_get_property_from_name_(klass, name);
    return property ? mono_property_get_get_method_(property) : nullptr;
}

MonoMethod* property_setter(MonoClass* klass, const char* name) {
    if (!klass)
        return nullptr;
    MonoProperty* property =
        mono_class_get_property_from_name_(klass, name);
    return property ? mono_property_get_set_method_(property) : nullptr;
}

MonoObject* managed_target(std::uint32_t handle) {
    return handle != 0 && mono_gchandle_get_target_
        ? mono_gchandle_get_target_(handle)
        : nullptr;
}

bool replace_managed_handle(
    std::uint32_t& handle,
    MonoObject* object) {

    if (!object || !mono_gchandle_new_ || !mono_gchandle_get_target_)
        return false;

    if (handle != 0 && managed_target(handle) == object)
        return true;

    if (handle != 0 && mono_gchandle_free_)
        mono_gchandle_free_(handle);

    handle = mono_gchandle_new_(object, 1);
    return handle != 0 && managed_target(handle) == object;
}

void release_managed_handle(std::uint32_t& handle) {
    if (handle != 0 && mono_gchandle_free_)
        mono_gchandle_free_(handle);
    handle = 0;
}

MonoObject* get_root_widget() {
    MonoObject* game_scene = managed_target(g_game_scene_handle);
    if (!game_scene)
        return nullptr;

    MonoClass* scene = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI.UI2",
        "Scene");
    MonoMethod* getter = property_getter(scene, "RootWidget");
    return getter ? invoke(getter, game_scene) : nullptr;
}

bool remove_child(MonoObject* root, MonoObject* child) {
    if (!root || !child || !g_pui_image || !mono_runtime_invoke_)
        return false;

    MonoClass* klass = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI.UI2",
        "Widget");
    MonoMethod* method = klass
        ? mono_class_get_method_from_name_(klass, "RemoveChild", 1)
        : nullptr;
    if (!method)
        return false;

    void* args[1] = {child};
    MonoObject* exception = nullptr;
    mono_runtime_invoke_(method, root, args, &exception);
    return exception == nullptr;
}

void remove_and_release_handle(MonoObject* root, std::uint32_t& handle) {
    if (handle != 0) {
        MonoObject* target = managed_target(handle);
        if (target && root) {
            remove_child(root, target);
        }
        release_managed_handle(handle);
    }
}

bool set_label_text(MonoObject* label, const char* text);   /* defined below */

/* Removing a widget does not visibly erase it on every firmware: on 13.40 the old copy stayed on
 * screen with frozen text. A label with empty text draws nothing, so blank before dropping. */
void prefbar_blank_rows() {
    for (auto h : g_telemetry_row_handle) {
        MonoObject* old = managed_target(h);
        if (old) set_label_text(old, "");
    }
    for (auto h : g_telemetry_shadow_handle) {
        MonoObject* old = managed_target(h);
        if (old) set_label_text(old, "");
    }
}

void prefbar_remove_rows(MonoObject* root) {
    static int logged = 0;
    if (logged++ < 10)
        log_line("prefbar: rows released (scene change)");
    prefbar_blank_rows();
    for (auto& h : g_telemetry_row_handle)
        remove_and_release_handle(root, h);
    for (auto& h : g_telemetry_shadow_handle)
        remove_and_release_handle(root, h);
    g_telemetry_layout.valid = false;
    g_telemetry_applied_count = 0;
    g_telemetry_hidden = false;
}

void prefbar_release_rows() {
    prefbar_blank_rows();
    for (auto& h : g_telemetry_row_handle)
        release_managed_handle(h);
    for (auto& h : g_telemetry_shadow_handle)
        release_managed_handle(h);
    g_telemetry_layout.valid = false;
    g_telemetry_applied_count = 0;
    g_telemetry_hidden = false;
}

bool refresh_game_scene() {
    if (!g_domain || !g_find_scene_method || !mono_string_new_)
        return false;

    MonoDomain* active_domain = mono_domain_get_();
    MonoString* game_path = mono_string_new_(
        active_domain ? active_domain : g_domain,
        "Game");
    void* find_args[1] = {game_path};
    MonoObject* game_scene = invoke(
        g_find_scene_method, nullptr, find_args);
    if (!game_scene)
        return false;

    if (managed_target(g_game_scene_handle) != game_scene) {
        MonoObject* old_root = get_root_widget();
        if (old_root) {
            prefbar_remove_rows(old_root);
        } else {
            prefbar_release_rows();
        }
        if (!replace_managed_handle(g_game_scene_handle, game_scene))
            return false;
        log_line(
            "Game ContainerScene refreshed scene=%p handle=%u mode=%s",
            static_cast<void*>(game_scene),
            g_game_scene_handle,
            g_background_render_mode ? "legacy-background" : "update-hook");
    }
    return true;
}

template <typename T>
bool set_property_direct(
    MonoClass* klass,
    MonoObject* instance,
    const char* name,
    T value) {

    MonoMethod* setter = property_setter(klass, name);
    if (!setter)
        return false;

    void* thunk = mono_compile_method_(setter);
    if (!thunk)
        return false;

    auto fn = reinterpret_cast<void (*)(MonoObject*, T)>(thunk);
    fn(instance, value);
    return true;
}

bool set_property_object(
    MonoClass* klass,
    MonoObject* instance,
    const char* name,
    MonoObject* value) {

    MonoMethod* setter = property_setter(klass, name);
    if (!setter)
        return false;

    void* args[1] = {value};
    MonoObject* exception = nullptr;
    mono_runtime_invoke_(setter, instance, args, &exception);
    return exception == nullptr;
}

MonoObject* create_font(int size) {
    MonoClass* klass = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI.UI2",
        "UIFont");
    if (!klass)
        return nullptr;

    MonoObject* boxed = mono_object_new_(g_domain, klass);
    if (!boxed)
        return nullptr;

    void* real = mono_object_unbox_(boxed);
    MonoMethod* ctor = mono_class_get_method_from_name_(klass, ".ctor", 3);
    void* thunk = ctor ? mono_compile_method_(ctor) : nullptr;
    if (!real || !thunk)
        return nullptr;

    auto fn = reinterpret_cast<void (*)(void*, int, int, int)>(thunk);
    fn(real, size, 0, 0);
    return reinterpret_cast<MonoObject*>(real);
}

MonoObject* create_color(float r, float g, float b, float a) {
    MonoClass* klass = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI",
        "UIColor");
    if (!klass)
        return nullptr;

    MonoObject* boxed = mono_object_new_(g_domain, klass);
    if (!boxed)
        return nullptr;

    void* real = mono_object_unbox_(boxed);
    MonoMethod* ctor = mono_class_get_method_from_name_(klass, ".ctor", 4);
    void* thunk = ctor ? mono_compile_method_(ctor) : nullptr;
    if (!real || !thunk)
        return nullptr;

    auto fn = reinterpret_cast<void (*)(void*, float, float, float, float)>(thunk);
    fn(real, r, g, b, a);
    return reinterpret_cast<MonoObject*>(real);
}

MonoObject* create_label(
    const char* name,
    float x,
    float y,
    const char* text,
    MonoObject* font,
    int horizontal_alignment,
    float r,
    float g,
    float b,
    float a) {

    MonoClass* klass = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI.UI2",
        "Label");
    if (!klass)
        return nullptr;

    MonoObject* label = mono_object_new_(g_domain, klass);
    if (!label)
        return nullptr;

    mono_runtime_object_init_(label);

    MonoString* mono_name = mono_string_new_(g_domain, name);
    MonoString* mono_text = mono_string_new_(g_domain, text);
    MonoObject* color = create_color(r, g, b, a);

    bool ok = true;
    ok &= set_property_direct(klass, label, "Name", mono_name);
    ok &= set_property_direct(klass, label, "X", x);
    ok &= set_property_direct(klass, label, "Y", y);
    ok &= set_property_direct(klass, label, "Text", mono_text);
    ok &= set_property_object(klass, label, "Font", font);
    ok &= set_property_direct(
        klass, label, "HorizontalAlignment", horizontal_alignment);
    ok &= set_property_direct(klass, label, "VerticalAlignment", 0);
    ok &= set_property_object(klass, label, "TextColor", color);
    ok &= set_property_direct(klass, label, "FitWidthToText", true);
    ok &= set_property_direct(klass, label, "FitHeightToText", true);

    return ok ? label : nullptr;
}

bool append_child(MonoObject* root, MonoObject* child) {
    MonoClass* klass = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI.UI2",
        "Widget");
    MonoMethod* method = klass
        ? mono_class_get_method_from_name_(klass, "AppendChild", 1)
        : nullptr;
    if (!method || !root || !child)
        return false;

    void* args[1] = {child};
    MonoObject* exception = nullptr;
    mono_runtime_invoke_(method, root, args, &exception);
    return exception == nullptr;
}

bool set_label_text(MonoObject* label, const char* text) {
    if (!label)
        return false;

    MonoClass* klass = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI.UI2",
        "Label");
    MonoString* mono_text = mono_string_new_(g_domain, text);
    return set_property_direct(klass, label, "Text", mono_text);
}

std::uint64_t prefbar_now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000u +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

/* receiver thread: validate, sanitise, publish */
void prefbar_ingest(const unsigned char* buf, long n) {
    static int rejected = 0;
    static int accepted = 0;
    if (n != static_cast<long>(sizeof(prefbar_wire_t))) {
        if (rejected++ < 5)
            log_line("prefbar: rx rejected size=%ld want=%lu", n,
                     static_cast<unsigned long>(sizeof(prefbar_wire_t)));
        return;
    }
    prefbar_wire_t pkt;
    std::memcpy(&pkt, buf, sizeof(pkt));
    if (!prefbar_wire_valid(&pkt, static_cast<std::size_t>(n))) {
        if (rejected++ < 5)
            log_line("prefbar: rx rejected invalid packet");
        return;
    }
    ++accepted;
    if (accepted <= 3 || accepted % 120 == 0)
        log_line("prefbar: rx ok #%d seq=%llu rows=%d",
                 accepted, static_cast<unsigned long long>(pkt.sequence), pkt.rows);
    for (int i = 0; i < PREFBAR_ROWS; ++i) {
        char* t = pkt.text[i];
        t[PREFBAR_ROW_LEN - 1] = '\0';
        for (char* c = t; *c; ++c) {
            const unsigned char u = static_cast<unsigned char>(*c);
            if (u < 0x20 || u > 0x7e)
                *c = ' ';   /* labels are single-line ASCII */
        }
    }
    pthread_mutex_lock(&g_telemetry_mutex);
    g_telemetry_packet = pkt;
    pthread_mutex_unlock(&g_telemetry_mutex);
    g_config_overlay_enabled.store((pkt.flags & PREFBAR_WIRE_FLAG_OVERLAY_ENABLED) != 0);
    g_touchpad_toggle_enabled.store((pkt.flags & PREFBAR_WIRE_FLAG_TOUCHPAD_TOGGLE) != 0);
    g_telemetry_rx_ms.store(prefbar_now_ms());
    g_telemetry_rx_count.fetch_add(1);
}

/* update hook: only touches widgets when something changed */
void prefbar_draw_text() {
    if (!g_runtime_ready.load())
        return;
    TelemetryDrawGuard guard(g_telemetry_draw_busy);
    if (!guard.owns)
        return;
    static bool announced = false;
    if (!announced) {
        announced = true;
        log_line("prefbar: draw loop active (status banner %s)",
                 kShowTelemetryStatus ? "on" : "off");
    }

    const std::uint64_t count = g_telemetry_rx_count.load();
    const bool have_data = count != 0;
    const bool stale = !have_data ||
        (prefbar_now_ms() - g_telemetry_rx_ms.load()) > kTelemetryStaleMs;
    if (count == g_telemetry_applied_count && stale == g_telemetry_hidden)
        return;

    prefbar_wire_t pkt;
    if (have_data) {
        pthread_mutex_lock(&g_telemetry_mutex);
        pkt = g_telemetry_packet;
        pthread_mutex_unlock(&g_telemetry_mutex);
    } else {
        std::memset(&pkt, 0, sizeof(pkt));
        /* same layout and colour as prefbar's defaults, so the first real packet only
         * changes text instead of forcing the labels to be rebuilt */
        pkt.corner = PREFBAR_CORNER_TL;
        pkt.rows = 3;
        pkt.font_size = 26;
        pkt.margin_x = 24.0f;
        pkt.margin_y = 24.0f;
        pkt.r = 0.95f; pkt.g = 0.95f; pkt.b = 0.95f; pkt.a = 1.0f;
    }

    /* with no fresh data show one status line instead of stale numbers */
    const char* banner = nullptr;
    const bool overlay_visible = g_prefbar_overlay_enabled &&
        g_config_overlay_enabled.load();
    if (overlay_visible && stale && kShowTelemetryStatus) {
        banner = have_data ? "prefbar: no data (prefbar.elf stopped?)"
                           : "prefbar-overlay ready - load prefbar.elf";
    }

    if (!refresh_game_scene()) {
        static int skipped = 0;
        if (skipped++ < 5)
            log_line("prefbar: draw skipped - game scene unavailable");
        return;
    }
    MonoObject* root = get_root_widget();
    if (!root) {
        static int no_root = 0;
        if (no_root++ < 5)
            log_line("prefbar: draw skipped - root widget unavailable");
        return;
    }

    const int font_px = pkt.font_size;
    const int corner = pkt.corner;
    const int rows = pkt.rows;
    const float mx = pkt.margin_x;
    const float my = pkt.margin_y;

    /* The row count only matters for the bottom-left anchor; ignoring it for top-left keeps the
     * labels from being rebuilt every time a sensor row appears or disappears. */
    const int layout_rows = (corner == PREFBAR_CORNER_BL) ? rows : 0;
    unsigned why = 0;
    if (!g_telemetry_layout.valid) why |= 1u;
    if (g_telemetry_layout.font != font_px) why |= 2u;
    if (g_telemetry_layout.corner != corner) why |= 4u;
    if (g_telemetry_layout.rows != layout_rows) why |= 8u;
    if (g_telemetry_layout.mx != mx || g_telemetry_layout.my != my) why |= 16u;
    if (g_telemetry_layout.r != pkt.r || g_telemetry_layout.g != pkt.g ||
        g_telemetry_layout.b != pkt.b) why |= 64u;
    const bool want_shadow = (pkt.flags & PREFBAR_WIRE_FLAG_TEXT_SHADOW) != 0;
    if (g_telemetry_layout.shadow != want_shadow) why |= 128u;
    MonoObject* labels[PREFBAR_ROWS]{};
    MonoObject* shadows[PREFBAR_ROWS]{};
    if (!why) {
        for (int i = 0; i < PREFBAR_ROWS; ++i) {
            labels[i] = managed_target(g_telemetry_row_handle[i]);
            if (want_shadow) shadows[i] = managed_target(g_telemetry_shadow_handle[i]);
            if (!labels[i] || (want_shadow && !shadows[i])) {
                why |= 32u;
                break;
            }
        }
    }
    const bool rebuild = why != 0;

    if (rebuild) {
        prefbar_blank_rows();
        for (auto& h : g_telemetry_row_handle)
            remove_and_release_handle(root, h);
        for (auto& h : g_telemetry_shadow_handle)
            remove_and_release_handle(root, h);
        MonoObject* font = create_font(font_px);
        if (!font) {
            log_line("prefbar: UIFont create failed");
            return;
        }
        const float line_h = static_cast<float>(font_px) * 1.3f;
        const float y0 = (corner == PREFBAR_CORNER_BL)
            ? 1080.0f - my - line_h * static_cast<float>(rows)
            : my;
        int created = 0;
        for (int i = 0; i < PREFBAR_ROWS; ++i) {
            char id[32];
            std::snprintf(id, sizeof(id), "id_prefbar_row%d", i);
            if (want_shadow) {
                char shadow_id[40];
                std::snprintf(shadow_id, sizeof(shadow_id), "id_prefbar_shadow%d", i);
                MonoObject* shadow = create_label(
                    shadow_id, mx + 1.5f, y0 + line_h * static_cast<float>(i) + 1.5f,
                    "", font, 0, 0.0f, 0.0f, 0.0f, 0.82f);
                if (shadow && append_child(root, shadow) &&
                    replace_managed_handle(g_telemetry_shadow_handle[i], shadow)) {
                    shadows[i] = shadow;
                } else {
                    shadows[i] = nullptr;
                }
            }
            MonoObject* label = create_label(
                id, mx, y0 + line_h * static_cast<float>(i), "", font, 0,
                pkt.r, pkt.g, pkt.b, pkt.a);
            if (label && append_child(root, label) &&
                replace_managed_handle(g_telemetry_row_handle[i], label) &&
                (!want_shadow || shadows[i])) {
                labels[i] = label;
                ++created;
            } else {
                labels[i] = nullptr;
            }
        }
        ++g_row_rebuild_count;
        log_line("prefbar: rows built #%d why=0x%x created=%d/%d font=%d corner=%d rows=%d",
                 g_row_rebuild_count, why, created, PREFBAR_ROWS, font_px, corner, rows);
        g_telemetry_layout = TelemetryLayout{
            font_px, corner, layout_rows, mx, my, pkt.r, pkt.g, pkt.b,
            want_shadow, created == PREFBAR_ROWS};
    }

    int applied = 0;
    for (int i = 0; i < PREFBAR_ROWS; ++i) {
        if (!labels[i])
            continue;
        const char* text = "";
        if (!overlay_visible)
            text = "";
        else if (banner)
            text = (i == 0) ? banner : "";
        else if (!stale && i < rows)
            text = pkt.text[i];
        if (want_shadow && shadows[i])
            (void)set_label_text(shadows[i], text);
        if (set_label_text(labels[i], text))
            ++applied;
    }
    static int logged = 0;
    if (logged++ < 5)
        log_line("prefbar: text applied rows=%d/%d count=%llu stale=%d banner=%d",
                 applied, PREFBAR_ROWS, static_cast<unsigned long long>(count),
                 stale ? 1 : 0, banner ? 1 : 0);
    g_telemetry_applied_count = count;
    g_telemetry_hidden = stale;
}

struct pad_state_t {
    std::uint32_t buttons{};
    bool valid{};
};

bool pad_read(pad_state_t* s) {
    s->valid = false;
    static int retry_cooldown = 0;
    static int consecutive_read_failures = 0;

    if (g_pad_init_state != 1) {
        if (++retry_cooldown < 30) {
            return false;
        }
        retry_cooldown = 0;

        int user = -1;
        int rc = sceUserServiceGetForegroundUser(&user);
        if (rc < 0 || user == -1 || static_cast<std::uint32_t>(user) == 0xffffffffu) {
            return false;
        }

        int h = scePadOpen(user, 0, 0, nullptr);
        if (h < 0)
            h = scePadGetHandle(user, 0, 0);

        if (h >= 0) {
            g_pad_handle = h;
            g_pad_user = user;
            g_pad_init_state = 1;
            consecutive_read_failures = 0;
            log_line("pad init: user=0x%x handle OK = 0x%x", user, h);
        } else {
            static int fail_log_count = 0;
            if (++fail_log_count <= 5) {
                log_line("pad init retry: user=0x%x scePadOpen=0x%x", user, h);
            }
            return false;
        }
    }

    static int user_check_counter = 0;
    if (++user_check_counter > 120) {
        user_check_counter = 0;
        int fg_user = -1;
        if (sceUserServiceGetForegroundUser(&fg_user) == 0 &&
            fg_user != -1 && static_cast<std::uint32_t>(fg_user) != 0xffffffffu &&
            fg_user != g_pad_user) {
            log_line("pad: foreground user changed 0x%x -> 0x%x, reopening pad", g_pad_user, fg_user);
            if (g_pad_handle >= 0) {
                scePadClose(g_pad_handle);
            }
            g_pad_handle = -1;
            g_pad_init_state = 0;
            return false;
        }
    }

    unsigned char raw[256];
    std::memset(raw, 0, sizeof raw);
    int read_rc = scePadReadState(g_pad_handle, raw);
    if (read_rc < 0) {
        if (++consecutive_read_failures > 15) {
            log_line("pad: read failed rc=0x%x for 15 frames, resetting handle 0x%x",
                     read_rc, g_pad_handle);
            if (g_pad_handle >= 0) {
                scePadClose(g_pad_handle);
            }
            g_pad_handle = -1;
            g_pad_init_state = 0;
            consecutive_read_failures = 0;
        }
        return false;
    }
    consecutive_read_failures = 0;

    std::uint32_t buttons;
    std::int32_t connected;
    std::memcpy(&buttons, raw + 0x00, 4);
    std::memcpy(&connected, raw + 0x4c, 4);

    if ((buttons & 0x80000000u) || connected == 0) {
        s->buttons = 0;
        s->valid = true;
        return true;
    }

    s->buttons = buttons;
    s->valid = true;
    return true;
}

/* Read the physical touchpad-click bit from the existing ShellUI pad reader.
 * Three press edges in a 1.5-second window toggle only the text overlay; FPS
 * sampling and the injected renderer stay resident. */
void process_prefbar_toggle_input() {
    if (!g_touchpad_toggle_enabled.load()) return;
    pad_state_t p{};
    if (!pad_read(&p) || !p.valid)
        return;

    constexpr std::uint32_t kTouchpadClickBit = 0x100000u;
    const bool down = (p.buttons & kTouchpadClickBit) != 0;
    if (g_touchpad_triple_click.update(down, prefbar_now_ms())) {
        g_prefbar_overlay_enabled = !g_prefbar_overlay_enabled;
        g_telemetry_applied_count = UINT64_MAX; /* force the next render pass to clear/redraw rows */

        const char* state = g_prefbar_overlay_enabled ? "ON" : "OFF";
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(PREFBAR_CONTROL_PORT);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const char* msg = g_prefbar_overlay_enabled
                ? "PBCTL1:OVERLAY:ON" : "PBCTL1:OVERLAY:OFF";
            const auto sent = sendto(fd, msg, std::strlen(msg), 0,
                reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (sent != static_cast<ssize_t>(std::strlen(msg)))
                log_line("input: triple touchpad click toggled overlay %s; debug event send failed errno=%d",
                         state, errno);
            else
                log_line("input: triple touchpad click toggled overlay %s; sampler notification queued", state);
            close(fd);
        } else {
            log_line("input: triple touchpad click toggled overlay %s; control socket failed errno=%d",
                     state, errno);
        }
    }
}

void application_update_hook(MonoObject* instance) {
    process_prefbar_toggle_input();
    prefbar_draw_text();

    if (g_application_update_original)
        g_application_update_original(instance);
}

bool install_update_hook(MonoClass* application_class) {
    record_stage("hook_lookup");
    MonoMethod* update = application_class
        ? mono_class_get_method_from_name_(
              application_class,
              "Update",
              0)
        : nullptr;
    record_stage("hook_compile");
    auto* address = update
        ? static_cast<std::uint8_t*>(mono_compile_method_(update))
        : nullptr;
    if (!address) {
        log_line("Application.Update compile failed");
        return false;
    }

    record_stage("hook_read_start");
    std::array<std::uint8_t, kAtomicPatchSize> expected{};
    if (!probe_hook_bytes(address, expected)) {
        return false;
    }
    record_stage("hook_read_ok");

    static constexpr std::uint8_t kAbsoluteJumpPrefix[6] = {
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
    };

    const bool eta_chain =
        std::memcmp(
            expected.data(),
            kAbsoluteJumpPrefix,
            sizeof(kAbsoluteJumpPrefix)) == 0;

    record_stage("hook_prepare");
    std::size_t displaced_size = kAbsoluteJumpSize;
    application_update_t original_call = nullptr;

    if (eta_chain) {
        std::uint64_t previous_destination = 0;
        std::memcpy(
            &previous_destination,
            expected.data() + sizeof(kAbsoluteJumpPrefix),
            sizeof(previous_destination));

        if (previous_destination ==
            reinterpret_cast<std::uint64_t>(&application_update_hook)) {
            if (g_application_update_original &&
                g_hook_install_confirmed) {
                log_line(
                    "Application.Update hook already online method=%p",
                    static_cast<void*>(address));
                return true;
            }
            log_line(
                "Application.Update belongs to another resident renderer "
                "method=%p",
                static_cast<void*>(address));
            return false;
        }

        if (previous_destination < 0x10000ULL ||
            previous_destination >= 0x0000800000000000ULL ||
            previous_destination == reinterpret_cast<std::uint64_t>(address)) {
            log_line(
                "Application.Update etaHEN destination invalid method=%p "
                "destination=%p",
                static_cast<void*>(address),
                reinterpret_cast<void*>(previous_destination));
            return false;
        }

        original_call = reinterpret_cast<application_update_t>(
            previous_destination);
    } else {
        displaced_size = native_patch_length(expected.data());
        if (displaced_size == 0) {
            log_line(
                "Application.Update native prologue rejected "
                "method=%p bytes="
                "%02x%02x%02x%02x%02x%02x%02x%02x"
                "%02x%02x%02x%02x%02x%02x%02x%02x",
                static_cast<void*>(address),
                expected[0], expected[1], expected[2], expected[3],
                expected[4], expected[5], expected[6], expected[7],
                expected[8], expected[9], expected[10], expected[11],
                expected[12], expected[13], expected[14], expected[15]);
            return false;
        }

        record_stage("hook_trampoline");
        if (!prepare_trampoline(
                expected.data(),
                displaced_size,
                address + displaced_size)) {
            log_line(
                "Application.Update trampoline prepare failed method=%p "
                "mode=native-stopped-mdbg displaced=%zu",
                static_cast<void*>(address),
                displaced_size);
            return false;
        }
        original_call = reinterpret_cast<application_update_t>(
            &prefbar_update_trampoline);
    }

    std::array<std::uint8_t, kAtomicPatchSize> desired = expected;
    encode_absolute_jump(
        desired.data(),
        reinterpret_cast<const void*>(&application_update_hook));
    for (std::size_t i = kAbsoluteJumpSize;
         i < displaced_size;
         ++i) {
        desired[i] = 0x90;
    }

    g_application_update_original = original_call;

    std::uint64_t nonce = 0;
    record_stage("hook_request");
    if (!publish_hook_request(
            address,
            expected,
            desired,
            displaced_size,
            original_call,
            nonce)) {
        log_line(
            "Application.Update request publish failed method=%p",
            static_cast<void*>(address));
        return false;
    }

    log_line(
        "Application.Update request ready mode=%s method=%p hook=%p "
        "original=%p displaced=%zu nonce=0x%llx",
        eta_chain ? "etahen-stopped-chain" : "native-stopped-mdbg",
        static_cast<void*>(address),
        reinterpret_cast<void*>(&application_update_hook),
        reinterpret_cast<void*>(original_call),
        displaced_size,
        static_cast<unsigned long long>(nonce));

    ShellUiHookAck ack{};
    record_stage("hook_ack_wait");
    if (!wait_for_native_hook_ack(getpid(), nonce, ack)) {
        log_line(
            "Application.Update request timeout method=%p "
            "nonce=0x%llx",
            static_cast<void*>(address),
            static_cast<unsigned long long>(nonce));
        return false;
    }

    if (ack.status != static_cast<std::int32_t>(
            ShellUiHookStatus::Success) ||
        ack.phase != static_cast<std::uint8_t>(ShellUiHookAckPhase::Patch) ||
        ack.verified == 0 || ack.detached == 0 ||
        ack.auth_restored == 0) {
        log_line(
            "Application.Update request failed method=%p "
            "status=%d read_rc=%d write_rc=%d verified=%u "
            "restored=%u detached=%u auth_restored=%u",
            static_cast<void*>(address),
            ack.status,
            ack.read_rc,
            ack.write_rc,
            static_cast<unsigned>(ack.verified),
            static_cast<unsigned>(ack.restored),
            static_cast<unsigned>(ack.detached),
            static_cast<unsigned>(ack.auth_restored));
        return false;
    }

    log_line(
        "Application.Update hook online method=%p "
        "mode=%s original=%p hook=%p "
        "displaced=%zu stopped_patch=1 verified=1",
        static_cast<void*>(address),
        eta_chain ? "etahen-stopped-chain" : "native-stopped-mdbg",
        reinterpret_cast<void*>(original_call),
        reinterpret_cast<void*>(&application_update_hook),
        displaced_size);
    g_hook_install_confirmed = true;
    record_stage("hook_confirmed");
    return true;
}

}

bool initialize_runtime() {
    if (g_runtime_ready.load())
        return true;

    if (g_hook_attempted)
        return false;

    record_stage("mono_root_call");
    g_domain = mono_get_root_domain_();
    if (!g_domain) {
        log_line("root domain failed");
        return false;
    }
    mono_thread_attach_(g_domain);

    record_stage("mono_attached");

    g_pui_image = open_image(kPuiDll);
    MonoImage* app_system = open_image(kAppSystemDll);
    if (!g_pui_image || !app_system) {
        log_line("managed image open failed pui=%p app=%p",
                 static_cast<void*>(g_pui_image),
                 static_cast<void*>(app_system));
        return false;
    }

    record_stage("managed_images_ready");

    MonoClass* layer_manager = mono_class_from_name_(
        app_system,
        "Sce.Vsh.ShellUI.AppSystem",
        "LayerManager");
    g_find_scene_method = layer_manager
        ? mono_class_get_method_from_name_(
              layer_manager,
              "FindContainerSceneByPath",
              1)
        : nullptr;
    if (!g_find_scene_method) {
        log_line("FindContainerSceneByPath unavailable");
        return false;
    }

    if (!refresh_game_scene()) {
        log_line("Game ContainerScene no disponible al iniciar; overlay armado, "
                 "esperando a que se abra un juego");
    }

    record_stage("scene_ready");

    g_hook_attempted = true;
    MonoClass* application_class = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI",
        "Application");
    record_stage("hook_setup");
    if (application_class && install_update_hook(application_class)) {
        g_background_render_mode = false;
        log_line("renderer backend selected mode=application-update-hook");
    } else {
        log_line("Application.Update hook failed or unavailable, falling back to legacy-guard");
        const MainThreadGuardResult guard = install_main_thread_guard();
        if (guard == MainThreadGuardResult::Installed) {
            g_background_render_mode = true;
            log_line(
                "renderer backend selected mode=legacy-background-pui "
                "Application.Update=untouched");
        } else {
            log_line(
                "renderer backend selection failed; fail-closed without "
                "Application.Update write");
            return false;
        }
    }

    g_runtime_ready.store(true);
    log_line(
        "runtime ready pid=%d scene_handle=%u gc_mode=pinned backend=%s",
        getpid(),
        g_game_scene_handle,
        g_background_render_mode
            ? "legacy-background-pui"
            : "application-update-hook");
    return true;
}

bool initialize_receiver() {
    if (g_receiver_fd >= 0)
        return true;

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_line("receiver socket failed");
        return false;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    timeval receive_timeout{};
    receive_timeout.tv_sec = 0;
    receive_timeout.tv_usec = 33000;
    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &receive_timeout,
            sizeof(receive_timeout)) < 0) {
        log_line("receiver timeout setup failed");
        close(fd);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PREFBAR_WIRE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(
            fd,
            reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)) < 0) {
        log_line("receiver bind failed port=%u", PREFBAR_WIRE_PORT);
        close(fd);
        return false;
    }

    g_receiver_fd = fd;
    log_line(
        "receiver ready port=%u mode=persistent-main-thread "
        "timeout_ms=2000 stale=1",
        PREFBAR_WIRE_PORT);
    return true;
}

[[noreturn]] void run_receiver_loop() {
    for (;;) {
        unsigned char buf[1024];
        const long n = static_cast<long>(recv(g_receiver_fd, buf, sizeof(buf), 0));
        if (n > 0)
            prefbar_ingest(buf, n);

        if (g_background_render_mode) {
            process_prefbar_toggle_input();
            prefbar_draw_text();
        }
    }
}

}
