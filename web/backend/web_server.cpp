/*
 * PrefBar web Control Center HTTP server.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The page is embedded into the controller at build time. The server exposes
 * only that page and a small, fixed API; it never maps arbitrary filesystem
 * paths to HTTP requests.
 */

#include "web_server.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" void prefbar_debug_event(
    int level, const char* key, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

extern const std::uint8_t prefbar_web_index_html[];
extern const std::size_t prefbar_web_index_html_size;

namespace prefbar::web {
namespace {

constexpr int kLogError = 1;
constexpr int kLogWarning = 2;
constexpr int kLogInfo = 3;

constexpr char kConfigPath[] = "/data/prefbar/prefbar.conf";
constexpr char kStatusPath[] = "/data/prefbar/status.kv";
constexpr char kControllerLogPath[] = "/data/prefbar/controller.log";
constexpr char kDebugLogPath[] = "/data/prefbar/debug.log";
constexpr char kDiagnosticPath[] = "/data/prefbar/diag.txt";

constexpr std::size_t kMaxHeaderBytes = 8 * 1024;
constexpr std::size_t kMaxBodyBytes = 16 * 1024;
constexpr std::size_t kMaxConfigBytes = 8 * 1024;
constexpr std::size_t kMaxStatusBytes = 8 * 1024;
constexpr std::size_t kMaxDiagnosticSectionBytes = 16 * 1024;
constexpr std::size_t kMaxDiagnosticResponseBytes = 52 * 1024;
constexpr unsigned kSocketTimeoutSeconds = 3;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stopping{false};
std::atomic<int> g_listener{-1};
pthread_t g_worker{};
bool g_worker_created = false;
pthread_mutex_t g_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;

enum class FileReadResult {
    Ok,
    Missing,
    TooLarge,
    Failed
};

struct HttpRequest {
    char method[8]{};
    char target[1024]{};
    char content_type[128]{};
    const char* body = nullptr;
    std::size_t body_size = 0;
};

enum class ValueKind {
    Boolean,
    Integer,
    Color,
    Choice
};

struct ConfigRule {
    const char* key;
    ValueKind kind;
    long minimum;
    long maximum;
    const char* choices;
};

constexpr ConfigRule kConfigRules[] = {
    {"notifications", ValueKind::Boolean, 0, 1, nullptr},
    {"startup_notification", ValueKind::Boolean, 0, 1, nullptr},
    {"error_notifications", ValueKind::Boolean, 0, 1, nullptr},
    {"warning_notifications", ValueKind::Boolean, 0, 1, nullptr},
    {"debug_notifications", ValueKind::Boolean, 0, 1, nullptr},
    {"debug_notify", ValueKind::Integer, 0, 4, nullptr},
    {"notify_sec", ValueKind::Integer, 0, 3600, nullptr},

    {"overlay", ValueKind::Boolean, 0, 1, nullptr},
    {"text_color", ValueKind::Color, 0, 0, nullptr},
    {"text_shadow", ValueKind::Boolean, 0, 1, nullptr},
    {"overlay_mode", ValueKind::Boolean, 0, 1, nullptr},
    {"overlay_font", ValueKind::Integer, 10, 96, nullptr},
    {"overlay_corner", ValueKind::Choice, 0, 0, "0|2"},
    {"overlay_x", ValueKind::Integer, 0, 1800, nullptr},
    {"overlay_y", ValueKind::Integer, 0, 1000, nullptr},
    {"overlay_format", ValueKind::Choice, 0, 0, "compact|classic"},
    {"overlay_horizontal", ValueKind::Boolean, 0, 1, nullptr},
    {"overlay_horizontal_spacing", ValueKind::Choice, 0, 0, "short|medium|long"},
    {"overlay_horizontal_align", ValueKind::Choice, 0, 0, "left|center|right"},

    {"sensors", ValueKind::Boolean, 0, 1, nullptr},
    {"show_fps", ValueKind::Boolean, 0, 1, nullptr},
    {"show_frametime", ValueKind::Boolean, 0, 1, nullptr},
    {"show_1percent_low", ValueKind::Boolean, 0, 1, nullptr},
    {"show_cpu_usage", ValueKind::Boolean, 0, 1, nullptr},
    {"show_cpu_temp", ValueKind::Boolean, 0, 1, nullptr},
    {"show_cpu_clock", ValueKind::Boolean, 0, 1, nullptr},
    {"show_gpu_temp", ValueKind::Boolean, 0, 1, nullptr},
    {"gpu_temp_label", ValueKind::Choice, 0, 0, "gpu|soc"},
    {"show_gpu_clock", ValueKind::Boolean, 0, 1, nullptr},
    {"show_fan_duty", ValueKind::Boolean, 0, 1, nullptr},
    {"show_ram", ValueKind::Boolean, 0, 1, nullptr},
    {"show_vram", ValueKind::Boolean, 0, 1, nullptr},
    {"show_power", ValueKind::Boolean, 0, 1, nullptr},

    {"touchpad_toggle", ValueKind::Boolean, 0, 1, nullptr},
    {"diag", ValueKind::Boolean, 0, 1, nullptr},
    {"poll_us", ValueKind::Integer, 500, 20000, nullptr}
};

constexpr std::size_t kConfigRuleCount =
    sizeof(kConfigRules) / sizeof(kConfigRules[0]);

void trim(char* text) noexcept {
    if (!text)
        return;

    char* begin = text;
    while (*begin && std::isspace(static_cast<unsigned char>(*begin)))
        ++begin;

    if (begin != text)
        std::memmove(text, begin, std::strlen(begin) + 1);

    std::size_t length = std::strlen(text);
    while (length > 0 &&
           std::isspace(static_cast<unsigned char>(text[length - 1]))) {
        text[--length] = '\0';
    }
}

bool valid_integer(const char* value, long minimum, long maximum) noexcept {
    if (!value || !value[0])
        return false;

    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' &&
        parsed >= minimum && parsed <= maximum;
}

bool valid_color(const char* value) noexcept {
    if (!value || std::strlen(value) != 7 || value[0] != '#')
        return false;

    for (std::size_t i = 1; i < 7; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

bool valid_choice(const char* value, const char* choices) noexcept {
    if (!value || !choices)
        return false;

    const std::size_t value_length = std::strlen(value);
    const char* current = choices;
    while (*current) {
        const char* separator = std::strchr(current, '|');
        const std::size_t choice_length = separator
            ? static_cast<std::size_t>(separator - current)
            : std::strlen(current);

        if (choice_length == value_length &&
            std::memcmp(current, value, value_length) == 0) {
            return true;
        }

        if (!separator)
            break;
        current = separator + 1;
    }
    return false;
}

const ConfigRule* find_config_rule(const char* key, std::size_t* index) noexcept {
    for (std::size_t i = 0; i < kConfigRuleCount; ++i) {
        if (std::strcmp(key, kConfigRules[i].key) == 0) {
            if (index)
                *index = i;
            return &kConfigRules[i];
        }
    }
    return nullptr;
}

bool valid_config_value(const ConfigRule& rule, const char* value) noexcept {
    switch (rule.kind) {
    case ValueKind::Boolean:
    case ValueKind::Integer:
        return valid_integer(value, rule.minimum, rule.maximum);
    case ValueKind::Color:
        return valid_color(value);
    case ValueKind::Choice:
        return valid_choice(value, rule.choices);
    }
    return false;
}

/* Require the complete current schema so a stale/partial request cannot
 * silently reset omitted options to telemetry defaults. */
bool validate_config_document(const char* data, std::size_t size) noexcept {
    if (!data || size == 0 || size > kMaxConfigBytes ||
        std::memchr(data, '\0', size) != nullptr) {
        return false;
    }

    bool seen[kConfigRuleCount]{};
    std::size_t seen_count = 0;
    std::size_t offset = 0;

    while (offset < size) {
        const char* newline = static_cast<const char*>(
            std::memchr(data + offset, '\n', size - offset));
        const std::size_t line_size = newline
            ? static_cast<std::size_t>(newline - (data + offset))
            : size - offset;
        if (line_size >= 256)
            return false;

        char line[256];
        std::memcpy(line, data + offset, line_size);
        line[line_size] = '\0';
        trim(line);

        offset += line_size + (newline ? 1u : 0u);

        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            const std::size_t section_length = std::strlen(line);
            if (section_length < 3 || line[section_length - 1] != ']')
                return false;
            continue;
        }

        char* equals = std::strchr(line, '=');
        if (!equals)
            return false;

        *equals++ = '\0';
        trim(line);
        trim(equals);
        if (!line[0] || !equals[0])
            return false;

        std::size_t rule_index = 0;
        const ConfigRule* rule = find_config_rule(line, &rule_index);
        if (!rule || seen[rule_index] || !valid_config_value(*rule, equals))
            return false;

        seen[rule_index] = true;
        ++seen_count;
    }

    return seen_count == kConfigRuleCount;
}

FileReadResult read_file(const char* path,
                         char* output,
                         std::size_t capacity,
                         std::size_t* output_size) noexcept {
    if (!path || !output || capacity == 0 || !output_size)
        return FileReadResult::Failed;

    *output_size = 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? FileReadResult::Missing : FileReadResult::Failed;

    struct stat st{};
    if (fstat(fd, &st) == 0 &&
        (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > capacity)) {
        close(fd);
        return FileReadResult::TooLarge;
    }

    std::size_t used = 0;
    bool too_large = false;
    bool failed = false;
    for (;;) {
        if (used == capacity) {
            char extra = '\0';
            ssize_t count;
            do {
                count = read(fd, &extra, 1);
            } while (count < 0 && errno == EINTR);

            if (count > 0)
                too_large = true;
            else if (count < 0)
                failed = true;
            break;
        }

        const ssize_t count = read(fd, output + used, capacity - used);
        if (count > 0) {
            used += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR)
            continue;

        failed = true;
        break;
    }

    const int close_result = close(fd);
    if (too_large)
        return FileReadResult::TooLarge;
    if (failed || close_result != 0)
        return FileReadResult::Failed;

    *output_size = used;
    return FileReadResult::Ok;
}

bool write_all(int fd, const char* data, std::size_t size) noexcept {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, data + written, size - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool write_config_atomically(const char* data, std::size_t size) noexcept {
    struct stat current{};
    mode_t mode = 0666;
    if (stat(kConfigPath, &current) == 0)
        mode = current.st_mode & 0777;

    constexpr char temporary_path[] =
        "/data/prefbar/prefbar.conf.web.tmp";

    const int fd = open(temporary_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return false;

    bool ok = fchmod(fd, mode) == 0 && write_all(fd, data, size);
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;

    if (ok && rename(temporary_path, kConfigPath) == 0) {
        const int directory_fd = open("/data/prefbar", O_RDONLY);
        if (directory_fd >= 0) {
            (void)fsync(directory_fd);
            close(directory_fd);
        }
        return true;
    }

    (void)unlink(temporary_path);
    return false;
}

bool send_all(int socket_fd, const char* data, std::size_t size) noexcept {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t count = send(socket_fd, data + sent, size - sent, 0);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void send_response(int socket_fd,
                   int status,
                   const char* reason,
                   const char* content_type,
                   const char* body,
                   std::size_t body_size,
                   bool head_only = false) noexcept {
    char header[768];
    const int header_size = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: PrefBar/0.3.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "\r\n",
        status, reason, content_type, body_size);

    if (header_size <= 0 ||
        static_cast<std::size_t>(header_size) >= sizeof(header)) {
        return;
    }

    if (!send_all(socket_fd, header, static_cast<std::size_t>(header_size)))
        return;
    if (!head_only && body && body_size)
        (void)send_all(socket_fd, body, body_size);
}

void send_error(int socket_fd,
                int status,
                const char* reason,
                const char* error_code) noexcept {
    char body[192];
    const int length = std::snprintf(
        body, sizeof(body), "{\"error\":\"%s\"}\n", error_code);
    if (length <= 0)
        return;
    send_response(socket_fd, status, reason, "application/json; charset=utf-8",
                  body, static_cast<std::size_t>(length));
}

bool copy_header_value(char* destination,
                       std::size_t destination_size,
                       const char* value) noexcept {
    if (!destination || destination_size == 0 || !value)
        return false;
    const std::size_t length = std::strlen(value);
    if (length >= destination_size)
        return false;
    std::memcpy(destination, value, length + 1);
    return true;
}

int read_request(int socket_fd,
                 char* storage,
                 std::size_t storage_size,
                 HttpRequest* request) noexcept {
    if (!storage || !request || storage_size < kMaxHeaderBytes + 1)
        return 400;

    std::size_t used = 0;
    char* header_end = nullptr;
    for (;;) {
        if (used >= storage_size - 1)
            return 431;

        const ssize_t count = recv(socket_fd, storage + used,
                                   storage_size - used - 1, 0);
        if (count == 0)
            return 400;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return 400;
        }

        used += static_cast<std::size_t>(count);
        storage[used] = '\0';
        header_end = std::strstr(storage, "\r\n\r\n");
        if (header_end)
            break;
        if (used >= kMaxHeaderBytes)
            return 431;
    }

    const std::size_t header_bytes =
        static_cast<std::size_t>(header_end - storage) + 4;
    if (header_bytes > kMaxHeaderBytes)
        return 431;

    char* first_line_end = std::strstr(storage, "\r\n");
    if (!first_line_end || first_line_end > header_end)
        return 400;
    *first_line_end = '\0';

    char version[16]{};
    if (std::sscanf(storage, "%7s %1023s %15s",
                    request->method, request->target, version) != 3) {
        return 400;
    }
    if (std::strncmp(version, "HTTP/1.", 7) != 0)
        return 400;

    std::size_t content_length = 0;
    bool have_content_length = false;
    bool unsupported_transfer_encoding = false;

    char* line = first_line_end + 2;
    while (line < header_end) {
        char* line_end = std::strstr(line, "\r\n");
        if (!line_end || line_end > header_end)
            line_end = header_end;
        *line_end = '\0';

        char* colon = std::strchr(line, ':');
        if (colon) {
            *colon++ = '\0';
            trim(line);
            trim(colon);

            if (strcasecmp(line, "Content-Length") == 0) {
                if (have_content_length || !colon[0])
                    return 400;
                errno = 0;
                char* end = nullptr;
                const unsigned long long parsed =
                    std::strtoull(colon, &end, 10);
                if (errno || end == colon || *end != '\0')
                    return 400;
                if (parsed > kMaxBodyBytes)
                    return 413;
                content_length = static_cast<std::size_t>(parsed);
                have_content_length = true;
            } else if (strcasecmp(line, "Content-Type") == 0) {
                if (!copy_header_value(request->content_type,
                                       sizeof(request->content_type), colon))
                    return 400;
            } else if (strcasecmp(line, "Transfer-Encoding") == 0 &&
                       strcasecmp(colon, "identity") != 0) {
                unsupported_transfer_encoding = true;
            }
        }

        if (line_end == header_end)
            break;
        line = line_end + 2;
    }

    if (unsupported_transfer_encoding)
        return 400;

    if (std::strcmp(request->method, "PUT") == 0 && !have_content_length)
        return 411;

    std::size_t received_body = used > header_bytes ? used - header_bytes : 0;
    while (received_body < content_length) {
        if (used >= storage_size - 1)
            return 413;
        const ssize_t count = recv(socket_fd, storage + used,
                                   storage_size - used - 1, 0);
        if (count == 0)
            return 400;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return 400;
        }
        used += static_cast<std::size_t>(count);
        received_body += static_cast<std::size_t>(count);
    }

    storage[header_bytes + content_length] = '\0';
    request->body = storage + header_bytes;
    request->body_size = content_length;

    char* query = std::strchr(request->target, '?');
    if (query)
        *query = '\0';

    if (request->target[0] != '/')
        return 400;

    return 0;
}

bool append_text(char* output,
                 std::size_t capacity,
                 std::size_t* used,
                 const char* text,
                 std::size_t length) noexcept {
    if (!output || !used || !text || *used > capacity ||
        length > capacity - *used) {
        return false;
    }
    std::memcpy(output + *used, text, length);
    *used += length;
    return true;
}

void append_diagnostic_section(char* output,
                               std::size_t capacity,
                               std::size_t* used,
                               const char* title,
                               const char* path) noexcept {
    char heading[96];
    const int heading_length = std::snprintf(
        heading, sizeof(heading), "\n===== %s =====\n", title);
    if (heading_length <= 0 ||
        !append_text(output, capacity, used, heading,
                     static_cast<std::size_t>(heading_length))) {
        return;
    }

    char contents[kMaxDiagnosticSectionBytes];
    std::size_t size = 0;
    const FileReadResult result = read_file(
        path, contents, sizeof(contents), &size);

    if (result == FileReadResult::Ok) {
        (void)append_text(output, capacity, used, contents, size);
        if (size && contents[size - 1] != '\n')
            (void)append_text(output, capacity, used, "\n", 1);
    } else {
        const char* message = result == FileReadResult::Missing
            ? "Not available.\n"
            : "Could not read this file (or it exceeded the size limit).\n";
        (void)append_text(output, capacity, used, message,
                          std::strlen(message));
    }
}

void handle_api_request(int socket_fd,
                        const HttpRequest& request,
                        bool head_only) noexcept {
    if (std::strcmp(request.method, "GET") == 0 ||
        std::strcmp(request.method, "HEAD") == 0) {
        if (std::strcmp(request.target, "/api/v1/health") == 0) {
            char body[192];
            const int length = std::snprintf(
                body, sizeof(body),
                "{\"ok\":true,\"service\":\"PrefBar\",\"version\":\"0.3.0\","
                "\"port\":%u}\n",
                static_cast<unsigned>(kDefaultPort));
            if (length > 0)
                send_response(socket_fd, 200, "OK",
                              "application/json; charset=utf-8", body,
                              static_cast<std::size_t>(length), head_only);
            return;
        }

        if (std::strcmp(request.target, "/api/v1/config") == 0) {
            char body[kMaxConfigBytes];
            std::size_t size = 0;
            const FileReadResult result = read_file(
                kConfigPath, body, sizeof(body), &size);
            if (result == FileReadResult::Ok) {
                send_response(socket_fd, 200, "OK",
                              "text/plain; charset=utf-8", body, size, head_only);
            } else if (result == FileReadResult::Missing) {
                send_error(socket_fd, 503, "Service Unavailable",
                           "configuration_not_ready");
            } else {
                send_error(socket_fd, 500, "Internal Server Error",
                           "configuration_read_failed");
            }
            return;
        }

        if (std::strcmp(request.target, "/api/v1/status") == 0) {
            char body[kMaxStatusBytes];
            std::size_t size = 0;
            const FileReadResult result = read_file(
                kStatusPath, body, sizeof(body), &size);
            if (result == FileReadResult::Ok) {
                send_response(socket_fd, 200, "OK",
                              "text/plain; charset=utf-8", body, size, head_only);
            } else if (result == FileReadResult::Missing) {
                send_error(socket_fd, 503, "Service Unavailable",
                           "status_not_ready");
            } else {
                send_error(socket_fd, 500, "Internal Server Error",
                           "status_read_failed");
            }
            return;
        }

        if (std::strcmp(request.target, "/api/v1/diagnostics") == 0) {
            char body[kMaxDiagnosticResponseBytes];
            std::size_t used = 0;
            append_diagnostic_section(body, sizeof(body), &used,
                                      "controller.log", kControllerLogPath);
            append_diagnostic_section(body, sizeof(body), &used,
                                      "debug.log", kDebugLogPath);
            append_diagnostic_section(body, sizeof(body), &used,
                                      "diag.txt", kDiagnosticPath);
            send_response(socket_fd, 200, "OK",
                          "text/plain; charset=utf-8", body, used, head_only);
            return;
        }

        send_error(socket_fd, 404, "Not Found", "not_found");
        return;
    }

    if (std::strcmp(request.method, "PUT") == 0) {
        if (std::strcmp(request.target, "/api/v1/config") != 0) {
            send_error(socket_fd, 404, "Not Found", "not_found");
            return;
        }
        if (strncasecmp(request.content_type, "text/plain", 10) != 0) {
            send_error(socket_fd, 415, "Unsupported Media Type",
                       "expected_text_plain");
            return;
        }
        if (!validate_config_document(request.body, request.body_size)) {
            send_error(socket_fd, 400, "Bad Request",
                       "invalid_or_incomplete_configuration");
            return;
        }

        pthread_mutex_lock(&g_config_mutex);
        const bool saved = write_config_atomically(
            request.body, request.body_size);
        pthread_mutex_unlock(&g_config_mutex);

        if (!saved) {
            prefbar_debug_event(kLogError, "web.config.save",
                                "Control Center configuration save failed (errno %d)",
                                errno);
            send_error(socket_fd, 500, "Internal Server Error",
                       "configuration_save_failed");
            return;
        }

        prefbar_debug_event(kLogInfo, "web.config.save",
                            "Control Center saved prefbar.conf; settings will be "
                            "reloaded when the next game session is confirmed");
        static constexpr char response[] =
            "{\"saved\":true,\"apply\":\"next_game_session\"}\n";
        send_response(socket_fd, 200, "OK",
                      "application/json; charset=utf-8",
                      response, sizeof(response) - 1);
        return;
    }

    send_error(socket_fd, 405, "Method Not Allowed", "method_not_allowed");
}

void handle_client(int client_fd) noexcept {
    int enabled = 1;
#ifdef SO_NOSIGPIPE
    (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE,
                     &enabled, sizeof(enabled));
#endif

    timeval timeout{};
    timeout.tv_sec = kSocketTimeoutSeconds;
    timeout.tv_usec = 0;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout));

    char storage[kMaxHeaderBytes + kMaxBodyBytes + 1];
    HttpRequest request{};
    const int parse_status = read_request(
        client_fd, storage, sizeof(storage), &request);
    if (parse_status != 0) {
        switch (parse_status) {
        case 411:
            send_error(client_fd, 411, "Length Required", "content_length_required");
            break;
        case 413:
            send_error(client_fd, 413, "Payload Too Large", "request_too_large");
            break;
        case 431:
            send_error(client_fd, 431, "Request Header Fields Too Large",
                       "headers_too_large");
            break;
        default:
            send_error(client_fd, 400, "Bad Request", "bad_request");
            break;
        }
        return;
    }

    const bool head_only = std::strcmp(request.method, "HEAD") == 0;
    if (std::strcmp(request.method, "GET") == 0 || head_only) {
        if (std::strcmp(request.target, "/") == 0 ||
            std::strcmp(request.target, "/index.html") == 0) {
            send_response(
                client_fd, 200, "OK", "text/html; charset=utf-8",
                reinterpret_cast<const char*>(prefbar_web_index_html),
                prefbar_web_index_html_size, head_only);
            return;
        }
    }

    if (std::strcmp(request.method, "GET") == 0 ||
        std::strcmp(request.method, "HEAD") == 0 ||
        std::strcmp(request.method, "PUT") == 0) {
        handle_api_request(client_fd, request, head_only);
        return;
    }

    send_error(client_fd, 405, "Method Not Allowed", "method_not_allowed");
}

void* server_worker(void*) noexcept {
    const int listener = g_listener.load(std::memory_order_acquire);
    while (!g_stopping.load(std::memory_order_acquire) && listener >= 0) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener, &read_set);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        const int ready = select(listener + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready == 0)
            continue;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            if (!g_stopping.load(std::memory_order_acquire)) {
                prefbar_debug_event(kLogWarning, "web.select",
                                    "HTTP listener select failed (errno %d)", errno);
            }
            break;
        }

        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (!g_stopping.load(std::memory_order_acquire)) {
                prefbar_debug_event(kLogWarning, "web.accept",
                                    "HTTP client accept failed (errno %d)", errno);
            }
            break;
        }

        handle_client(client);
        close(client);
    }

    g_running.store(false, std::memory_order_release);
    return nullptr;
}

} // namespace

int start() noexcept {
    pthread_mutex_lock(&g_lifecycle_mutex);

    if (g_worker_created) {
        const bool running = g_running.load(std::memory_order_acquire);
        pthread_mutex_unlock(&g_lifecycle_mutex);
        return running ? 0 : EALREADY;
    }

    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        const int error = errno ? errno : EIO;
        pthread_mutex_unlock(&g_lifecycle_mutex);
        return error;
    }

    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) != 0) {
        const int error = errno ? errno : EIO;
        close(listener);
        pthread_mutex_unlock(&g_lifecycle_mutex);
        return error;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDefaultPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        const int error = errno ? errno : EIO;
        close(listener);
        pthread_mutex_unlock(&g_lifecycle_mutex);
        return error;
    }

    g_stopping.store(false, std::memory_order_release);
    g_listener.store(listener, std::memory_order_release);
    g_running.store(true, std::memory_order_release);

    const int thread_error = pthread_create(
        &g_worker, nullptr, server_worker, nullptr);
    if (thread_error != 0) {
        g_listener.store(-1, std::memory_order_release);
        g_running.store(false, std::memory_order_release);
        close(listener);
        pthread_mutex_unlock(&g_lifecycle_mutex);
        return thread_error;
    }

    g_worker_created = true;
    pthread_mutex_unlock(&g_lifecycle_mutex);

    prefbar_debug_event(kLogInfo, "web.start",
                        "PrefBar Control Center listening on port %u (all IPv4 interfaces, unrestricted clients)",
                        static_cast<unsigned>(kDefaultPort));
    return 0;
}

void stop() noexcept {
    pthread_mutex_lock(&g_lifecycle_mutex);
    if (!g_worker_created) {
        pthread_mutex_unlock(&g_lifecycle_mutex);
        return;
    }

    g_stopping.store(true, std::memory_order_release);
    const int listener = g_listener.load(std::memory_order_acquire);
    const pthread_t worker = g_worker;
    pthread_mutex_unlock(&g_lifecycle_mutex);

    if (listener >= 0)
        (void)shutdown(listener, SHUT_RDWR);
    (void)pthread_join(worker, nullptr);

    pthread_mutex_lock(&g_lifecycle_mutex);
    if (listener >= 0)
        close(listener);
    g_listener.store(-1, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
    g_worker_created = false;
    pthread_mutex_unlock(&g_lifecycle_mutex);

    prefbar_debug_event(kLogInfo, "web.stop", "PrefBar Control Center stopped");
}

bool is_running() noexcept {
    return g_running.load(std::memory_order_acquire);
}

} // namespace prefbar::web
