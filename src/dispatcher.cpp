#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <chrono>

// POSIX socket headers — available in Android NDK
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <android/log.h>

#include "config.h"
#include "ring_buffer.h"

// ─────────────────────────────────────────────────────────────────────────────
// Convenience logging macros — confined to this translation unit
// ─────────────────────────────────────────────────────────────────────────────
#define DISP_LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO,  passive_observer::kLogTag, __VA_ARGS__)
#define DISP_LOGW(...) \
    __android_log_print(ANDROID_LOG_WARN,  passive_observer::kLogTag, __VA_ARGS__)
#define DISP_LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, passive_observer::kLogTag, __VA_ARGS__)
#define DISP_LOGD(...) \
    __android_log_print(ANDROID_LOG_DEBUG, passive_observer::kLogTag, __VA_ARGS__)

namespace passive_observer {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations (internal linkage)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/// Opens a blocking TCP socket to kServerHost:kServerPort.
/// Returns a valid fd on success, -1 on failure.
[[nodiscard]]
int  ConnectToServer() noexcept;

/// Configures SO_KEEPALIVE, TCP_NODELAY, and send/recv timeouts.
void ConfigureSocket(int fd) noexcept;

/// Sends exactly `len` bytes from `buf` over `fd`.
/// Returns true on success, false on any partial write or error.
[[nodiscard]]
bool SendAll(int fd, const uint8_t* buf, size_t len) noexcept;

/// Formats a RingSlot into a JSON line written into `out_buf`.
/// Returns the number of bytes written (excluding NUL), 0 on error.
[[nodiscard]]
size_t FormatEventJson(const RingSlot& slot,
                       char*           out_buf,
                       size_t          out_buf_size) noexcept;

/// Maps EventType enum to a human-readable C-string.
[[nodiscard]]
const char* EventTypeName(EventType type) noexcept;

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// DispatcherState — shared state between public API and the worker thread
// ─────────────────────────────────────────────────────────────────────────────
struct DispatcherState {
    std::atomic<bool>   running   { false };
    std::atomic<bool>   shutdown  { false };
    std::thread         worker;
};

static DispatcherState g_dispatcher;

// ─────────────────────────────────────────────────────────────────────────────
// Dispatcher worker — main loop
// ─────────────────────────────────────────────────────────────────────────────
static void DispatcherWorker() noexcept {
    DISP_LOGI("Dispatcher thread started. Target=%s:%u",
              kServerHost, static_cast<unsigned>(kServerPort));

    // Per-iteration scratch buffer for JSON serialisation.
    // Stack-allocated; size = kMaxEventPayloadSize * 2 covers hex-escaped
    // worst-case payload + JSON envelope overhead.
    static constexpr size_t kJsonBufSize = kMaxEventPayloadSize * 2u + 256u;
    char json_buf[kJsonBufSize];

    RingSlot slot{};

    int      sock_fd       = -1;
    uint32_t fail_streak   = 0u;
    uint64_t events_sent   = 0u;

    while (!g_dispatcher.shutdown.load(std::memory_order_acquire)) {

        // ── 1. Ensure we have a live connection ───────────────────────────
        if (sock_fd < 0) {
            if (fail_streak >= kMaxSendFailures) {
                // Exponential-ish back-off: wait before retrying.
                DISP_LOGW("Dispatcher: %u consecutive failures. "
                          "Backing off %u ms.",
                          fail_streak, kReconnectDelayMs);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kReconnectDelayMs));
                fail_streak = 0u;
            }

            sock_fd = ConnectToServer();
            if (sock_fd < 0) {
                ++fail_streak;
                continue;
            }
            ConfigureSocket(sock_fd);
            fail_streak = 0u;
            DISP_LOGI("Dispatcher: connected to %s:%u (fd=%d)",
                      kServerHost,
                      static_cast<unsigned>(kServerPort),
                      sock_fd);
        }

        // ── 2. Drain the ring buffer ──────────────────────────────────────
        bool made_progress = false;

        while (!g_dispatcher.shutdown.load(std::memory_order_relaxed)) {

            if (!GetRingBuffer().TryDequeue(slot)) {
                // Buffer empty — break inner loop, go to idle sleep.
                break;
            }

            made_progress = true;

            // Serialise event to JSON line.
            const size_t json_len =
                FormatEventJson(slot, json_buf, kJsonBufSize);

            if (json_len == 0u) {
                DISP_LOGW("Dispatcher: FormatEventJson returned 0; skipping.");
                continue;
            }

            // Append newline delimiter so Python server can readline().
            if (json_len + 1u < kJsonBufSize) {
                json_buf[json_len]      = '\n';
                // SendAll with json_len + 1 to include the newline.
                if (!SendAll(sock_fd,
                             reinterpret_cast<const uint8_t*>(json_buf),
                             json_len + 1u)) {
                    DISP_LOGE("Dispatcher: SendAll failed (fd=%d): %s",
                              sock_fd, std::strerror(errno));
                    ::close(sock_fd);
                    sock_fd = -1;
                    ++fail_streak;
                    // Re-enqueue is not possible in SPSC — event is lost.
                    // Log the drop and break to reconnect.
                    break;
                }
                ++events_sent;

                if ((events_sent & 0xFFu) == 0u) {
                    // Periodic heartbeat log every 256 events.
                    DISP_LOGD("Dispatcher: %" PRIu64 " events sent | "
                              "~%zu buffered | %" PRIu64 " dropped",
                              events_sent,
                              GetRingBuffer().ApproxSize(),
                              GetRingBuffer().DroppedCount());
                }
            } else {
                DISP_LOGW("Dispatcher: json_buf overflow guard triggered.");
            }
        }

        // ── 3. Idle sleep when buffer was empty ───────────────────────────
        if (!made_progress) {
            ::usleep(kDispatcherIdleSleepUs);
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    if (sock_fd >= 0) {
        ::shutdown(sock_fd, SHUT_RDWR);
        ::close(sock_fd);
        sock_fd = -1;
    }

    DISP_LOGI("Dispatcher thread exiting. Total sent=%" PRIu64
              " | Total dropped=%" PRIu64,
              events_sent,
              GetRingBuffer().DroppedCount());
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Starts the background dispatcher thread.
/// Must be called exactly once from JNI_OnLoad after the ring buffer is ready.
/// Returns true on success.
bool StartDispatcher() noexcept {
    bool expected = false;
    if (!g_dispatcher.running.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        DISP_LOGW("StartDispatcher: already running.");
        return false;
    }

    g_dispatcher.shutdown.store(false, std::memory_order_release);

    try {
        g_dispatcher.worker = std::thread(DispatcherWorker);
    } catch (const std::exception& ex) {
        DISP_LOGE("StartDispatcher: thread creation failed: %s", ex.what());
        g_dispatcher.running.store(false, std::memory_order_release);
        return false;
    }

    DISP_LOGI("StartDispatcher: worker thread launched.");
    return true;
}

/// Signals the dispatcher to stop and blocks until the thread exits.
/// Safe to call multiple times.
void StopDispatcher() noexcept {
    if (!g_dispatcher.running.load(std::memory_order_acquire)) {
        return;
    }

    DISP_LOGI("StopDispatcher: signalling shutdown.");
    g_dispatcher.shutdown.store(true, std::memory_order_release);

    if (g_dispatcher.worker.joinable()) {
        g_dispatcher.worker.join();
    }

    g_dispatcher.running.store(false, std::memory_order_release);
    DISP_LOGI("StopDispatcher: complete.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers — anonymous namespace
// ─────────────────────────────────────────────────────────────────────────────
namespace {

int ConnectToServer() noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        DISP_LOGE("ConnectToServer: socket() failed: %s", std::strerror(errno));
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(kServerPort);

    if (::inet_pton(AF_INET, kServerHost, &addr.sin_addr) != 1) {
        DISP_LOGE("ConnectToServer: inet_pton() failed for host '%s'",
                  kServerHost);
        ::close(fd);
        return -1;
    }

    if (::connect(fd,
                  reinterpret_cast<const struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        DISP_LOGW("ConnectToServer: connect() failed: %s",
                  std::strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

void ConfigureSocket(int fd) noexcept {
    // TCP_NODELAY — disable Nagle; we want low-latency small writes.
    int flag = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     &flag, sizeof(flag)) < 0) {
        DISP_LOGW("ConfigureSocket: TCP_NODELAY failed: %s",
                  std::strerror(errno));
    }

    // SO_KEEPALIVE — detect dead connections.
    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                     &flag, sizeof(flag)) < 0) {
        DISP_LOGW("ConfigureSocket: SO_KEEPALIVE failed: %s",
                  std::strerror(errno));
    }

    // SO_SNDTIMEO — prevent indefinite blocking on send().
    struct timeval tv{};
    tv.tv_sec  = static_cast<time_t>(kSocketTimeoutSec);
    tv.tv_usec = 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     &tv, sizeof(tv)) < 0) {
        DISP_LOGW("ConfigureSocket: SO_SNDTIMEO failed: %s",
                  std::strerror(errno));
    }

    // SO_RCVTIMEO — symmetric timeout for any future reads.
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     &tv, sizeof(tv)) < 0) {
        DISP_LOGW("ConfigureSocket: SO_RCVTIMEO failed: %s",
                  std::strerror(errno));
    }
}

bool SendAll(int fd, const uint8_t* buf, size_t len) noexcept {
    size_t total_sent = 0u;
    while (total_sent < len) {
        const ssize_t n = ::send(fd,
                                 buf + total_sent,
                                 len - total_sent,
                                 MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(n);
    }
    return true;
}

const char* EventTypeName(EventType type) noexcept {
    switch (type) {
        case EventType::kNetConnect: return "net_connect";
        case EventType::kNetSend:    return "net_send";
        case EventType::kNetRecv:    return "net_recv";
        case EventType::kSslWrite:   return "ssl_write";
        case EventType::kSslRead:    return "ssl_read";
        default:                     return "unknown";
    }
}

size_t FormatEventJson(const RingSlot& slot,
                       char*           out_buf,
                       size_t          out_buf_size) noexcept {
    // Hex-encode the raw payload for safe JSON transport.
    // Each byte → 2 hex chars; buffer must fit hex + envelope.
    static constexpr size_t kHexBufSize = kMaxEventPayloadSize * 2u + 1u;
    char hex_buf[kHexBufSize];

    const uint32_t plen = slot.payload_len;
    size_t         hpos = 0u;

    for (uint32_t i = 0u; i < plen && hpos + 2u < kHexBufSize; ++i) {
        static constexpr char kHexChars[] = "0123456789abcdef";
        hex_buf[hpos++] = kHexChars[(slot.payload[i] >> 4u) & 0x0Fu];
        hex_buf[hpos++] = kHexChars[ slot.payload[i]        & 0x0Fu];
    }
    hex_buf[hpos] = '\0';

    // Build JSON line:
    // {"type":"<name>","payload_len":<N>,"payload":"<hex>"}
    const int written = std::snprintf(
        out_buf,
        out_buf_size,
        "{\"type\":\"%s\",\"payload_len\":%u,\"payload\":\"%s\"}",
        EventTypeName(slot.event_type),
        static_cast<unsigned>(plen),
        hex_buf
    );

    if (written <= 0 || static_cast<size_t>(written) >= out_buf_size) {
        return 0u;
    }

    return static_cast<size_t>(written);
}

} // anonymous namespace
} // namespace passive_observer
