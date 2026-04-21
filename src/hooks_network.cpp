#include "hook_manager.h"
#include "ring_buffer.h"
#include "config.h"

#include <cstring>
#include <cerrno>
#include <cstdint>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/log.h>

// bhook (ByteHook) header
#include <bytehook.h>

#define LOG_TAG "Observer::NetHooks"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace observer {

// ─────────────────────────────────────────────
// bhook stub handles - kept for uninstall
// ─────────────────────────────────────────────
static bytehook_stub_t g_stub_connect = nullptr;
static bytehook_stub_t g_stub_send    = nullptr;
static bytehook_stub_t g_stub_recv    = nullptr;

// ─────────────────────────────────────────────
// Original function typedefs
// ─────────────────────────────────────────────
using connect_fn_t = int(*)(int, const struct sockaddr*, socklen_t);
using send_fn_t    = ssize_t(*)(int, const void*, size_t, int);
using recv_fn_t    = ssize_t(*)(int, void*, size_t, int);

// ─────────────────────────────────────────────
// Stack-allocated JSON-like serialisation
// Produces: {"type":"X","ip":"...","port":N,"fd":N}
// NO malloc, NO heap - uses fixed stack buffer
// ─────────────────────────────────────────────
static uint32_t serialise_connect(
    uint8_t*                out,       // stack buffer provided by caller
    uint32_t                out_size,
    int                     fd,
    const struct sockaddr*  addr) noexcept
{
    char ip_str[INET6_ADDRSTRLEN] = "<unknown>";
    uint16_t port = 0;

    if (addr) {
        if (addr->sa_family == AF_INET) {
            const auto* a4 = reinterpret_cast<const struct sockaddr_in*>(addr);
            ::inet_ntop(AF_INET, &a4->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(a4->sin_port);
        } else if (addr->sa_family == AF_INET6) {
            const auto* a6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
            ::inet_ntop(AF_INET6, &a6->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(a6->sin6_port);
        }
    }

    int written = ::snprintf(
        reinterpret_cast<char*>(out), out_size,
        R"({"event":"NET_CONNECT","fd":%d,"ip":"%s","port":%u})",
        fd, ip_str, port);

    if (written < 0 || static_cast<uint32_t>(written) >= out_size) {
        return 0;
    }
    return static_cast<uint32_t>(written);
}

static uint32_t serialise_send_recv(
    uint8_t*        out,
    uint32_t        out_size,
    const char*     event_name,
    int             fd,
    const uint8_t*  data,
    size_t          data_len) noexcept
{
    // Hex-encode up to 64 bytes of payload for readability
    constexpr size_t MAX_HEX_BYTES = 64;
    const size_t hex_bytes = (data_len < MAX_HEX_BYTES) ? data_len : MAX_HEX_BYTES;

    // 2 hex chars per byte + null
    char hex_buf[MAX_HEX_BYTES * 2 + 1] = {};
    static constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < hex_bytes; ++i) {
        hex_buf[i * 2 + 0] = HEX[(data[i] >> 4) & 0xF];
        hex_buf[i * 2 + 1] = HEX[(data[i])      & 0xF];
    }

    int written = ::snprintf(
        reinterpret_cast<char*>(out), out_size,
        R"({"event":"%s","fd":%d,"len":%zu,"hex":"%s","truncated":%s})",
        event_name,
        fd,
        data_len,
        hex_buf,
        (data_len > MAX_HEX_BYTES) ? "true" : "false");

    if (written < 0 || static_cast<uint32_t>(written) >= out_size) {
        return 0;
    }
    return static_cast<uint32_t>(written);
}

// ─────────────────────────────────────────────
// Hook: connect(2)
// STRICT: NO malloc, NO I/O, NO blocking
// ─────────────────────────────────────────────
static int hook_connect(int fd,
                        const struct sockaddr* addr,
                        socklen_t addrlen) noexcept {
    // Call original FIRST via bhook trampoline
    BYTEHOOK_STACK_SCOPE();
    int ret = BYTEHOOK_CALL_PREV(hook_connect, connect_fn_t,
                                 fd, addr, addrlen);

    // Only log successful or in-progress connections
    if (ret == 0 || errno == EINPROGRESS) {
        // Stack-allocated serialisation buffer
        uint8_t buf[256];
        uint32_t len = serialise_connect(buf, sizeof(buf), fd, addr);
        if (len > 0) {
            get_ring_buffer().push(config::EventType::NET_CONNECT, buf, len);
        }
    }

    return ret;
}

// ─────────────────────────────────────────────
// Hook: send(2)
// ─────────────────────────────────────────────
static ssize_t hook_send(int fd,
                         const void* buf,
                         size_t len,
                         int flags) noexcept {
    BYTEHOOK_STACK_SCOPE();
    ssize_t ret = BYTEHOOK_CALL_PREV(hook_send, send_fn_t,
                                     fd, buf, len, flags);

    if (ret > 0) {
        uint8_t serial_buf[512];
        uint32_t serial_len = serialise_send_recv(
            serial_buf, sizeof(serial_buf),
            "NET_SEND",
            fd,
            reinterpret_cast<const uint8_t*>(buf),
            static_cast<size_t>(ret));

        if (serial_len > 0) {
            get_ring_buffer().push(config::EventType::NET_SEND,
                                   serial_buf, serial_len);
        }
    }

    return ret;
}

// ─────────────────────────────────────────────
// Hook: recv(2)
// ─────────────────────────────────────────────
static ssize_t hook_recv(int fd,
                         void* buf,
                         size_t len,
                         int flags) noexcept {
    BYTEHOOK_STACK_SCOPE();
    ssize_t ret = BYTEHOOK_CALL_PREV(hook_recv, recv_fn_t,
                                     fd, buf, len, flags);

    if (ret > 0) {
        uint8_t serial_buf[512];
        uint32_t serial_len = serialise_send_recv(
            serial_buf, sizeof(serial_buf),
            "NET_RECV",
            fd,
            reinterpret_cast<const uint8_t*>(buf),
            static_cast<size_t>(ret));

        if (serial_len > 0) {
            get_ring_buffer().push(config::EventType::NET_RECV,
                                   serial_buf, serial_len);
        }
    }

    return ret;
}

// ─────────────────────────────────────────────
// Public: install_network_hooks()
// ─────────────────────────────────────────────
bool install_network_hooks() noexcept {
    // Hook connect in all loaded libs + libc
    g_stub_connect = bytehook_hook_all(
        nullptr,             // all callers
        "connect",
        reinterpret_cast<void*>(hook_connect),
        nullptr, nullptr);

    g_stub_send = bytehook_hook_all(
        nullptr,
        "send",
        reinterpret_cast<void*>(hook_send),
        nullptr, nullptr);

    g_stub_recv = bytehook_hook_all(
        nullptr,
        "recv",
        reinterpret_cast<void*>(hook_recv),
        nullptr, nullptr);

    if (!g_stub_connect || !g_stub_send || !g_stub_recv) {
        LOGE("One or more network hooks failed to install. "
             "connect=%p send=%p recv=%p",
             g_stub_connect, g_stub_send, g_stub_recv);
        return false;
    }

    LOGI("Network hooks installed: connect=%p send=%p recv=%p",
         g_stub_connect, g_stub_send, g_stub_recv);
    return true;
}

// ─────────────────────────────────────────────
// Public: uninstall_all_hooks()
// Called from main.cpp destructor / JNI_OnUnload
// ─────────────────────────────────────────────
void uninstall_all_hooks() noexcept {
    if (g_stub_connect) {
        bytehook_unhook(g_stub_connect);
        g_stub_connect = nullptr;
    }
    if (g_stub_send) {
        bytehook_unhook(g_stub_send);
        g_stub_send = nullptr;
    }
    if (g_stub_recv) {
        bytehook_unhook(g_stub_recv);
        g_stub_recv = nullptr;
    }
    LOGI("Network hooks uninstalled.");
}

} // namespace observer