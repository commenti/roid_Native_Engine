#include "ring_buffer.h"
#include "config.h"

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <atomic>
#include <thread>
#include <chrono>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>

#define LOG_TAG "Observer::Dispatcher"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

namespace observer {

// ─────────────────────────────────────────────
// Internal State
// ─────────────────────────────────────────────
static std::atomic<bool>  g_dispatcher_running{false};
static std::atomic<int>   g_sockfd{-1};
static std::thread        g_dispatcher_thread;

// Static output buffer - single consumer, no malloc needed
// Size: LogPacket header + max payload + framing overhead (4-byte length prefix)
static constexpr size_t FRAME_BUF_SIZE =
    sizeof(uint32_t) +                    // 4-byte network length prefix
    sizeof(config::LogPacket) +
    config::MAX_PAYLOAD_SIZE;

// ─────────────────────────────────────────────
// TCP Connection Helper
// Returns valid fd on success, -1 on failure
// ─────────────────────────────────────────────
static int connect_to_server() noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket() failed: %s", ::strerror(errno));
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(config::SERVER_PORT);

    if (::inet_pton(AF_INET, config::SERVER_IP, &addr.sin_addr) != 1) {
        LOGE("inet_pton() failed for IP: %s", config::SERVER_IP);
        ::close(fd);
        return -1;
    }

    if (::connect(fd,
                  reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        LOGE("connect() failed: %s", ::strerror(errno));
        ::close(fd);
        return -1;
    }

    LOGI("Connected to %s:%u", config::SERVER_IP, config::SERVER_PORT);
    return fd;
}

// ─────────────────────────────────────────────
// Reliable send: retries on partial writes
// Returns true only if all `len` bytes sent
// ─────────────────────────────────────────────
static bool send_all(int fd, const uint8_t* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd,
                           data + sent,
                           len - sent,
                           MSG_NOSIGNAL); // suppress SIGPIPE
        if (n <= 0) {
            LOGE("send() failed: %s (sent %zu / %zu)", ::strerror(errno), sent, len);
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// ─────────────────────────────────────────────
// Dispatcher Thread Body
// Protocol: [4-byte LE frame_len][LogPacket header][raw payload]
// ─────────────────────────────────────────────
static void dispatcher_loop() noexcept {
    // Per-thread static buffers - no heap allocation
    static uint8_t pop_buf[sizeof(config::LogPacket) + config::MAX_PAYLOAD_SIZE];
    static uint8_t send_buf[FRAME_BUF_SIZE];

    RingBuffer& rb = get_ring_buffer();

    while (g_dispatcher_running.load(std::memory_order_acquire)) {

        // ── Ensure socket is connected ──────────────────────
        int fd = g_sockfd.load(std::memory_order_relaxed);
        if (fd < 0) {
            fd = connect_to_server();
            if (fd < 0) {
                // Retry after backoff
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            g_sockfd.store(fd, std::memory_order_relaxed);
        }

        // ── Drain ring buffer in tight loop ─────────────────
        bool had_data = false;
        while (true) {
            size_t frame_len = rb.pop(pop_buf, sizeof(pop_buf));
            if (frame_len == 0) {
                break; // Empty - yield below
            }

            had_data = true;

            // Build length-prefixed frame
            // 4-byte little-endian frame length (excludes the 4-byte prefix itself)
            uint32_t net_len = static_cast<uint32_t>(frame_len);
            ::memcpy(send_buf,                    &net_len,  sizeof(uint32_t));
            ::memcpy(send_buf + sizeof(uint32_t), pop_buf,   frame_len);

            const size_t total_send = sizeof(uint32_t) + frame_len;

            if (!send_all(fd, send_buf, total_send)) {
                // Socket broken - close and reconnect next iteration
                LOGW("Socket broken, will reconnect.");
                ::close(fd);
                g_sockfd.store(-1, std::memory_order_relaxed);
                break; // Exit inner drain loop
            }
        }

        // ── Yield / Sleep when buffer is empty ──────────────
        if (!had_data) {
            // Short sleep to avoid busy-spin burning CPU
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // ── Cleanup on shutdown ──────────────────────────────
    int fd = g_sockfd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        ::close(fd);
        g_sockfd.store(-1, std::memory_order_relaxed);
    }

    LOGI("Dispatcher thread exited.");
}

// ─────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────

// Called from JNI_OnLoad
void dispatcher_start() noexcept {
    bool expected = false;
    if (!g_dispatcher_running.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        LOGW("Dispatcher already running.");
        return;
    }

    g_sockfd.store(-1, std::memory_order_relaxed);

    g_dispatcher_thread = std::thread(dispatcher_loop);

    LOGI("Dispatcher thread started.");
}

// Called from JNI_OnUnload or destructor
void dispatcher_stop() noexcept {
    g_dispatcher_running.store(false, std::memory_order_release);

    if (g_dispatcher_thread.joinable()) {
        g_dispatcher_thread.join();
    }

    LOGI("Dispatcher stopped.");
}

} // namespace observer