/*
 * PrefBar Media shortcut installer.
 * Copyright (C) 2026 X1NONs
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "app_launcher.hpp"
#include "web_server.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#ifndef PREFBAR_LAUNCHER_TITLE_ID
#error "PREFBAR_LAUNCHER_TITLE_ID must be configured by CMake"
#endif
#ifndef PREFBAR_LAUNCHER_PORT
#error "PREFBAR_LAUNCHER_PORT must be configured by CMake"
#endif

extern "C" int sceAppInstUtilInitialize(void);
extern "C" int sceAppInstUtilTerminate(void);
extern "C" int sceAppInstUtilAppInstallAll(void*);

extern "C" void prefbar_debug_event(
    int level, const char* key, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

extern const std::uint8_t prefbar_launcher_param_json[];
extern const std::size_t prefbar_launcher_param_json_size;
extern const std::uint8_t prefbar_launcher_icon0_png[];
extern const std::size_t prefbar_launcher_icon0_png_size;

namespace prefbar::launcher {
namespace {

constexpr int kLogError = 1;
constexpr int kLogWarning = 2;
constexpr int kLogInfo = 3;

constexpr char kTitleId[] = PREFBAR_LAUNCHER_TITLE_ID;
constexpr char kAppRoot[] = "/user/app";
constexpr char kDataRoot[] = "/data/prefbar";
constexpr char kOwnerMarker[] = "PrefBar Media shortcut owner: " PREFBAR_LAUNCHER_TITLE_ID "\n";
constexpr char kInstalledMarker[] = "PrefBar Media shortcut registered: " PREFBAR_LAUNCHER_TITLE_ID "\n";
constexpr unsigned kInstallAttempts = 3;
constexpr useconds_t kRetryDelayUs = 1'500'000;

static_assert(sizeof(kTitleId) - 1 == 9,
              "The PS5 homebrew Title ID must be nine characters");
static_assert(prefbar::web::kDefaultPort == PREFBAR_LAUNCHER_PORT,
              "The shortcut deeplink port must match the Control Center port");

std::atomic<bool> g_worker_active{false};
std::atomic<bool> g_installed{false};

enum class InstallResult {
    Success,
    Retry,
    Conflict
};

enum class EntryType {
    Missing,
    RegularFile,
    Directory,
    Other,
    Error
};

EntryType entry_type(const char* path) noexcept {
    struct stat st{};
    if (lstat(path, &st) == 0) {
        if (S_ISREG(st.st_mode))
            return EntryType::RegularFile;
        if (S_ISDIR(st.st_mode))
            return EntryType::Directory;
        return EntryType::Other;
    }
    return errno == ENOENT ? EntryType::Missing : EntryType::Error;
}

bool ensure_directory(const char* path, mode_t mode) noexcept {
    const EntryType before = entry_type(path);
    if (before == EntryType::Directory)
        return true;
    if (before != EntryType::Missing)
        return false;

    if (mkdir(path, mode) == 0)
        return true;
    if (errno != EEXIST)
        return false;
    return entry_type(path) == EntryType::Directory;
}

bool file_matches(const char* path,
                  const std::uint8_t* expected,
                  std::size_t expected_size) noexcept {
    struct stat before{};
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) != expected_size) {
        return false;
    }

    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;

    struct stat opened{};
    bool same = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
        opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
        opened.st_size >= 0 &&
        static_cast<std::uint64_t>(opened.st_size) == expected_size;

    std::uint8_t buffer[2048];
    std::size_t offset = 0;
    while (same && offset < expected_size) {
        ssize_t count;
        do {
            count = read(fd, buffer,
                         expected_size - offset < sizeof(buffer)
                             ? expected_size - offset
                             : sizeof(buffer));
        } while (count < 0 && errno == EINTR);

        if (count <= 0 ||
            std::memcmp(buffer, expected + offset,
                        static_cast<std::size_t>(count)) != 0) {
            same = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }

    if (same) {
        std::uint8_t extra = 0;
        ssize_t count;
        do {
            count = read(fd, &extra, 1);
        } while (count < 0 && errno == EINTR);
        same = count == 0;
    }

    if (close(fd) != 0)
        same = false;
    return same;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool write_file_atomic(const char* path,
                       const std::uint8_t* data,
                       std::size_t size) noexcept {
    const EntryType destination_type = entry_type(path);
    if (destination_type != EntryType::Missing &&
        destination_type != EntryType::RegularFile) {
        return false;
    }

    char temporary_path[512]{};
    const int temp_length = std::snprintf(
        temporary_path, sizeof(temporary_path), "%s.prefbar-tmp", path);
    if (temp_length <= 0 ||
        static_cast<std::size_t>(temp_length) >= sizeof(temporary_path)) {
        errno = ENAMETOOLONG;
        return false;
    }

    const EntryType temp_type = entry_type(temporary_path);
    if (temp_type == EntryType::RegularFile) {
        if (unlink(temporary_path) != 0)
            return false;
    } else if (temp_type != EntryType::Missing) {
        errno = EEXIST;
        return false;
    }

    const int fd = open(temporary_path,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;

    bool ok = write_all(fd, data, size);
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;

    if (ok && rename(temporary_path, path) == 0)
        return true;

    (void)unlink(temporary_path);
    return false;
}

bool ensure_marker(const char* path,
                  const char* marker,
                  std::size_t marker_size) noexcept {
    if (file_matches(path,
                     reinterpret_cast<const std::uint8_t*>(marker),
                     marker_size)) {
        return true;
    }

    const EntryType type = entry_type(path);
    if (type != EntryType::Missing && type != EntryType::RegularFile)
        return false;

    return write_file_atomic(
        path, reinterpret_cast<const std::uint8_t*>(marker), marker_size) &&
        file_matches(path,
                     reinterpret_cast<const std::uint8_t*>(marker),
                     marker_size);
}

InstallResult install_launcher() noexcept {
    if (!prefbar::web::is_running())
        return InstallResult::Retry;

    char app_dir[128]{};
    char sys_dir[160]{};
    char param_path[192]{};
    char icon_path[192]{};
    char owner_path[192]{};
    char installed_path[192]{};

    const int app_length = std::snprintf(
        app_dir, sizeof(app_dir), "%s/%s", kAppRoot, kTitleId);
    const int sys_length = std::snprintf(
        sys_dir, sizeof(sys_dir), "%s/sce_sys", app_dir);
    const int param_length = std::snprintf(
        param_path, sizeof(param_path), "%s/param.json", sys_dir);
    const int icon_length = std::snprintf(
        icon_path, sizeof(icon_path), "%s/icon0.png", sys_dir);
    const int owner_length = std::snprintf(
        owner_path, sizeof(owner_path), "%s/launcher.owner", kDataRoot);
    const int installed_length = std::snprintf(
        installed_path, sizeof(installed_path), "%s/launcher.installed", kDataRoot);

    if (app_length <= 0 || static_cast<std::size_t>(app_length) >= sizeof(app_dir) ||
        sys_length <= 0 || static_cast<std::size_t>(sys_length) >= sizeof(sys_dir) ||
        param_length <= 0 || static_cast<std::size_t>(param_length) >= sizeof(param_path) ||
        icon_length <= 0 || static_cast<std::size_t>(icon_length) >= sizeof(icon_path) ||
        owner_length <= 0 || static_cast<std::size_t>(owner_length) >= sizeof(owner_path) ||
        installed_length <= 0 ||
        static_cast<std::size_t>(installed_length) >= sizeof(installed_path)) {
        prefbar_debug_event(kLogError, "launcher.path",
                            "Media shortcut path construction failed");
        return InstallResult::Retry;
    }

    if (!ensure_directory(kDataRoot, 0777) ||
        !ensure_directory(kAppRoot, 0755)) {
        prefbar_debug_event(kLogWarning, "launcher.directory",
                            "Could not access PrefBar launcher directories (errno %d)",
                            errno);
        return InstallResult::Retry;
    }

    const EntryType app_type = entry_type(app_dir);
    if (app_type == EntryType::Other || app_type == EntryType::Error) {
        prefbar_debug_event(kLogError, "launcher.collision",
                            "Refusing to use %s: it is not a normal app directory",
                            app_dir);
        return InstallResult::Conflict;
    }

    const bool owner_matches = file_matches(
        owner_path,
        reinterpret_cast<const std::uint8_t*>(kOwnerMarker),
        sizeof(kOwnerMarker) - 1);
    const EntryType owner_type = entry_type(owner_path);
    if (owner_type == EntryType::Other || owner_type == EntryType::Directory ||
        owner_type == EntryType::Error) {
        prefbar_debug_event(kLogError, "launcher.marker",
                            "Refusing unsafe ownership marker at %s", owner_path);
        return InstallResult::Conflict;
    }

    const EntryType sys_dir_type = entry_type(sys_dir);
    if (app_type == EntryType::Directory &&
        sys_dir_type != EntryType::Missing &&
        sys_dir_type != EntryType::Directory) {
        prefbar_debug_event(kLogError, "launcher.collision",
                            "Refusing unsafe metadata directory at %s", sys_dir);
        return InstallResult::Conflict;
    }

    const bool param_matches = file_matches(
        param_path, prefbar_launcher_param_json,
        prefbar_launcher_param_json_size);
    const bool icon_matches = file_matches(
        icon_path, prefbar_launcher_icon0_png,
        prefbar_launcher_icon0_png_size);
    const bool assets_already_registered =
        app_type == EntryType::Directory && param_matches && icon_matches;

    if (app_type == EntryType::Directory) {
        const EntryType param_type = entry_type(param_path);
        const EntryType icon_type = entry_type(icon_path);
        if (!owner_matches &&
            (sys_dir_type != EntryType::Directory ||
             !param_matches || !icon_matches)) {
            prefbar_debug_event(
                kLogError, "launcher.collision",
                "Refusing to overwrite existing app directory %s; Title ID %s is already in use",
                app_dir, kTitleId);
            return InstallResult::Conflict;
        }
        if (owner_matches &&
            ((param_type != EntryType::Missing && !param_matches) ||
             (icon_type != EntryType::Missing && !icon_matches))) {
            prefbar_debug_event(
                kLogError, "launcher.collision",
                "PrefBar's ownership marker exists, but %s contains different launcher assets; refusing to overwrite",
                app_dir);
            return InstallResult::Conflict;
        }
    }

    if (!ensure_marker(owner_path, kOwnerMarker, sizeof(kOwnerMarker) - 1)) {
        prefbar_debug_event(kLogWarning, "launcher.owner",
                            "Could not persist launcher ownership marker (errno %d)",
                            errno);
        return InstallResult::Retry;
    }

    if (!ensure_directory(app_dir, 0755) ||
        !ensure_directory(sys_dir, 0755)) {
        prefbar_debug_event(kLogWarning, "launcher.app_directory",
                            "Could not create PrefBar app metadata directory (errno %d)",
                            errno);
        return InstallResult::Retry;
    }

    if (!param_matches &&
        !write_file_atomic(param_path, prefbar_launcher_param_json,
                           prefbar_launcher_param_json_size)) {
        prefbar_debug_event(kLogWarning, "launcher.param",
                            "Could not write PrefBar param.json (errno %d)", errno);
        return InstallResult::Retry;
    }
    if (!icon_matches &&
        !write_file_atomic(icon_path, prefbar_launcher_icon0_png,
                           prefbar_launcher_icon0_png_size)) {
        prefbar_debug_event(kLogWarning, "launcher.icon",
                            "Could not write PrefBar icon0.png (errno %d)", errno);
        return InstallResult::Retry;
    }

    if (!file_matches(param_path, prefbar_launcher_param_json,
                      prefbar_launcher_param_json_size) ||
        !file_matches(icon_path, prefbar_launcher_icon0_png,
                      prefbar_launcher_icon0_png_size)) {
        prefbar_debug_event(kLogWarning, "launcher.verify",
                            "PrefBar launcher assets failed post-write verification");
        return InstallResult::Retry;
    }

    if (assets_already_registered && file_matches(
            installed_path,
            reinterpret_cast<const std::uint8_t*>(kInstalledMarker),
            sizeof(kInstalledMarker) - 1)) {
        g_installed.store(true, std::memory_order_release);
        return InstallResult::Success;
    }

    if (!prefbar::web::is_running())
        return InstallResult::Retry;

    const int init_result = sceAppInstUtilInitialize();
    if (init_result != 0) {
        prefbar_debug_event(kLogWarning, "launcher.appinst.initialize",
                            "AppInstUtil initialization failed (0x%08X)",
                            static_cast<unsigned>(init_result));
        return InstallResult::Retry;
    }

    using InstallTitleDir = int (*)(const char*, const char*, void*);
    InstallTitleDir install_title_dir = nullptr;
    std::uint32_t module_handle = 0;
    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &module_handle) == 0) {
        install_title_dir = reinterpret_cast<InstallTitleDir>(
            kernel_dynlib_resolve(-1, module_handle, "Wudg3Xe3heE"));
    }

    const bool used_title_dir = install_title_dir != nullptr;
    const int install_result = used_title_dir
        ? install_title_dir(kTitleId, "/user/app/", nullptr)
        : sceAppInstUtilAppInstallAll(nullptr);

    const int terminate_result = sceAppInstUtilTerminate();
    if (terminate_result != 0) {
        prefbar_debug_event(kLogWarning, "launcher.appinst.terminate",
                            "AppInstUtil termination returned 0x%08X",
                            static_cast<unsigned>(terminate_result));
    }

    if (install_result != 0) {
        prefbar_debug_event(
            kLogWarning, "launcher.appinst.install",
            "PS5 shortcut registration failed using %s (0x%08X); will retry",
            used_title_dir ? "AppInstallTitleDir" : "AppInstallAll",
            static_cast<unsigned>(install_result));
        return InstallResult::Retry;
    }

    if (!ensure_marker(installed_path, kInstalledMarker,
                       sizeof(kInstalledMarker) - 1)) {
        prefbar_debug_event(
            kLogWarning, "launcher.marker",
            "Shortcut registered, but its completion marker could not be saved (errno %d)",
            errno);
    }

    prefbar_debug_event(
        kLogInfo, "launcher.ready",
        "PrefBar Media shortcut registered (Title ID %s, port %u, %s)",
        kTitleId, static_cast<unsigned>(prefbar::web::kDefaultPort),
        used_title_dir ? "AppInstallTitleDir" : "AppInstallAll fallback");

    g_installed.store(true, std::memory_order_release);
    return InstallResult::Success;
}

void* launcher_worker(void*) noexcept {
    InstallResult result = InstallResult::Retry;
    for (unsigned attempt = 0; attempt < kInstallAttempts; ++attempt) {
        if (!prefbar::web::is_running())
            break;

        result = install_launcher();
        if (result != InstallResult::Retry)
            break;
        if (attempt + 1 < kInstallAttempts)
            (void)usleep(kRetryDelayUs);
    }

    if (result == InstallResult::Conflict) {
        prefbar_debug_event(kLogError, "launcher.aborted",
                            "PrefBar Media shortcut installation was stopped because the Title ID is occupied");
    } else if (result == InstallResult::Retry && prefbar::web::is_running()) {
        prefbar_debug_event(kLogWarning, "launcher.retry.exhausted",
                            "PrefBar Media shortcut could not be registered after %u attempts; it will be retried when the Control Center restarts",
                            kInstallAttempts);
    }

    g_worker_active.store(false, std::memory_order_release);
    return nullptr;
}

} // namespace

void request_install() noexcept {
    if (!prefbar::web::is_running() ||
        g_installed.load(std::memory_order_acquire)) {
        return;
    }

    bool expected = false;
    if (!g_worker_active.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    pthread_attr_t attributes{};
    int result = pthread_attr_init(&attributes);
    if (result == 0) {
        result = pthread_attr_setdetachstate(
            &attributes, PTHREAD_CREATE_DETACHED);
        if (result == 0) {
            pthread_t worker{};
            result = pthread_create(&worker, &attributes,
                                    launcher_worker, nullptr);
        }
        (void)pthread_attr_destroy(&attributes);
    }

    if (result != 0) {
        g_worker_active.store(false, std::memory_order_release);
        prefbar_debug_event(kLogWarning, "launcher.thread",
                            "Could not start Media shortcut installer thread (rc=%d)",
                            result);
    }
}

} // namespace prefbar::launcher
