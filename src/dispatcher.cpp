#include "config.h"
#include "ring_buffer.h"
#include <android/log.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cerrno>

namespace observer::dispatcher {
namespace {
    std::atomic<bool> s_running{false};
    std::atomic<int>  s_socket{-1};
    std::thread       s_worker;

    bool establish_connection(int& fd) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(config::SERVER_PORT);
        if (inet_pton(AF_INET, config::SERVER_HOST, &addr.sin_addr) != 1) {
            close(fd);
            return false;
        }

        struct timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = config::TCP_TIMEOUT_MS * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            fd = -1;
            return false;
        }
        return true;
    }

    bool dispatch_to_server(int fd, const uint8_t* payload, uint32_t len) {
        if (len == 0 || fd < 0) return false;

        // Prepend length as big-endian 32-bit integer        uint32_t net_len = htonl(len);
        size_t total = sizeof(net_len) + len;

        struct pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLOUT;

        if (poll(&pfd, 1, config::TCP_TIMEOUT_MS) <= 0) {
            return false;
        }

        const uint8_t* cur = reinterpret_cast<const uint8_t*>(&net_len);
        size_t remaining = total;

        while (remaining > 0) {
            ssize_t sent = send(fd, cur, remaining, MSG_NOSIGNAL);
            if (sent <= 0) return false;
            cur += sent;
            remaining -= sent;
        }
        return true;
    }
} // anonymous namespace

void thread_loop() {
    int current_sock = -1;
    OBS_LOG(ANDROID_LOG_INFO, "Dispatcher thread started.");

    while (s_running.load(std::memory_order_relaxed)) {
        if (current_sock < 0) {
            if (!establish_connection(current_sock)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            OBS_LOG(ANDROID_LOG_DEBUG, "TCP connection established to %s:%d", 
                    config::SERVER_HOST, config::SERVER_PORT);
        }

        core::RingBufferEvent event{};
        if (!core::g_ring_buffer.pop(event)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (!dispatch_to_server(current_sock, event.data, event.length)) {
            OBS_LOG(ANDROID_LOG_WARN, "Send failed, resetting socket.");
            close(current_sock);
            current_sock = -1;
        }
    }
    if (current_sock >= 0) close(current_sock);
    OBS_LOG(ANDROID_LOG_INFO, "Dispatcher thread stopped.");
}

void start() {
    if (s_running.exchange(true)) return;
    s_worker = std::thread(thread_loop);
}

void stop() {
    if (!s_running.exchange(false)) return;
    if (s_worker.joinable()) {
        s_worker.join();
    }
}
} // namespace observer::dispatcher
