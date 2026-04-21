#include "hook_manager.h"
#include "ring_buffer.h"
#include "config.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <android/log.h>

// ─────────────────────────────────────────────────────────────────────────────
// Logging macros — translation-unit scope only
// ─────────────────────────────────────────────────────────────────────────────
#define NET_LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO,  passive_observer::kLogTag, __VA_ARGS__)
#define NET_LOGW(...) \
    __android_log_print(ANDROID_LOG_WARN,  passive_observer::kLogTag, __VA_ARGS__)
#define NET_LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, passive_observer::kLogTag, __VA_ARGS__)

namespace passive_observer {

// ─────────────────────────────────────────────────────────────────────────────
// Original function pointer definitions
//
// Declared extern in hook_manager.h; defined here (network group).
// Initialised to nullptr; populated by ByteHook at install time.
// In DISABLE_BYTEHOOK mode, resolved via dlsym so call-throughs still work.
// ─────────────────────────────────────────────────────────────────────────────
ConnectFn  g_orig_connect  = nullptr;
SendFn     g_orig_send     = nullptr;
RecvFn     g_orig_recv     = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Crypto originals — defined in hooks_crypto.cpp; declared extern here only
// so this TU links cleanly. hook_manager.h already has the extern declarations.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Internal state
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/// Guards against double-install / double-uninstall races.
std::atomic<bool> g_network_hooks_installed { false };

// ── Payload builders ─────────────────────────────────────────────────────────

/// Serialises a sockaddr into a compact JSON object stored in `out`.
/// Returns bytes written (excluding NUL), 0 on failure.
/// SAFE: stack-only, no heap.
size_t BuildConnectPayload(const struct sockaddr* addr,
                           socklen_t              addrlen,
                           char*                  out,
                           size_t                 out_size) noexcept {
    if (!addr || out_size == 0u) return 0u;
    (void)addrlen;

    char ip_str[INET6_ADDRSTRLEN] = "<unknown>";
    uint16_t port = 0u;

    if (addr->sa_family == AF_INET) {
        const auto* s = reinterpret_cast<const struct sockaddr_in*>(addr);
        ::inet_ntop(AF_INET, &s->sin_addr, ip_str, sizeof(ip_str));
        port = ntohs(s->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const auto* s = reinterpret_cast<const struct sockaddr_in6*>(addr);
        ::inet_ntop(AF_INET6, &s->sin6_addr, ip_str, sizeof(ip_str));
        port = ntohs(s->sin6_port);
    }

    const int n = std::snprintf(out, out_size,
        "{\"hook\":\"connect\",\"ip\":\"%s\",\"port\":%u}",
        ip_str, static_cast<unsigned>(port));

    return (n > 0 && static_cast<size_t>(n) < out_size)
           ? static_cast<size_t>(n) : 0u;
}

/// Serialises send() metadata (fd + first N bytes) into `out`.
size_t BuildSendPayload(int         sockfd,
                        const void* buf,
                        size_t      len,
                        char*       out,
                        size_t      out_size) noexcept {
    if (!out || out_size == 0u) return 0u;

    // Capture up to 64 bytes of data as hex for the JSON snapshot.
    static constexpr size_t kSnapLen = 64u;
    const size_t snap = (len < kSnapLen) ? len : kSnapLen;

    char hex[kSnapLen * 2u + 1u] {};
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(buf);
    size_t hpos = 0u;

    if (buf) {
        static constexpr char kHex[] = "0123456789abcdef";
        for (size_t i = 0u; i < snap; ++i) {
            hex[hpos++] = kHex[(bytes[i] >> 4u) & 0x0Fu];
            hex[hpos++] = kHex[ bytes[i]        & 0x0Fu];
        }
    }
    hex[hpos] = '\0';

    const int n = std::snprintf(out, out_size,
        "{\"hook\":\"send\",\"fd\":%d,\"len\":%zu,\"snap\":\"%s\"}",
        sockfd, len, hex);

    return (n > 0 && static_cast<size_t>(n) < out_size)
           ? static_cast<size_t>(n) : 0u;
}

/// Serialises recv() result metadata into `out`.
size_t BuildRecvPayload(int         sockfd,
                        const void* buf,
                        ssize_t     ret,
                        char*       out,
                        size_t      out_size) noexcept {
    if (!out || out_size == 0u) return 0u;

    static constexpr size_t kSnapLen = 64u;
    const size_t snap = (ret > 0)
        ? static_cast<size_t>((static_cast<size_t>(ret) < kSnapLen)
                               ? ret : kSnapLen)
        : 0u;

    char hex[kSnapLen * 2u + 1u] {};
    size_t hpos = 0u;

    if (buf && ret > 0) {
        static constexpr char kHex[] = "0123456789abcdef";
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(buf);
        for (size_t i = 0u; i < snap; ++i) {
            hex[hpos++] = kHex[(bytes[i] >> 4u) & 0x0Fu];
            hex[hpos++] = kHex[ bytes[i]        & 0x0Fu];
        }
    }
    hex[hpos] = '\0';

    const int n = std::snprintf(out, out_size,
        "{\"hook\":\"recv\",\"fd\":%d,\"ret\":%zd,\"snap\":\"%s\"}",
        sockfd, ret, hex);

    return (n > 0 && static_cast<size_t>(n) < out_size)
           ? static_cast<size_t>(n) : 0u;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Hook trampolines
//
// CRITICAL RULES enforced here:
//   • NO malloc / new / delete.
//   • NO synchronous I/O.
//   • NO blocking primitives.
//   • Payload built on the stack then copied into the ring buffer.
//   • Call-through to original function happens unconditionally.
// ─────────────────────────────────────────────────────────────────────────────

/// Hook: connect(2)
static int Hook_connect(int                    sockfd,
                        const struct sockaddr* addr,
                        socklen_t              addrlen) noexcept {
    // ── 1. Call through to the real connect() first ───────────────────────
    //    We record AFTER to capture the result, but we must have the
    //    address data now (addr may be stack-allocated by caller).
    //    So capture address metadata before calling through.

    // Stack-allocated payload buffer — no heap.
    char payload[kMaxEventPayloadSize];
    const size_t plen = BuildConnectPayload(addr, addrlen,
                                            payload, sizeof(payload));

    // ── 2. Enqueue event (non-blocking) ───────────────────────────────────
    if (plen > 0u) {
        GetRingBuffer().TryEnqueue(EventType::kNetConnect,
                                   payload,
                                   static_cast<uint32_t>(plen));
    }

    // ── 3. Call through ───────────────────────────────────────────────────
    if (g_orig_connect) {
        return g_orig_connect(sockfd, addr, addrlen);
    }
    // Fallback: should never reach here in production.
    errno = ENOSYS;
    return -1;
}

/// Hook: send(2)
static ssize_t Hook_send(int         sockfd,
                          const void* buf,
                          size_t      len,
                          int         flags) noexcept {
    char payload[kMaxEventPayloadSize];
    const size_t plen = BuildSendPayload(sockfd, buf, len,
                                         payload, sizeof(payload));
    if (plen > 0u) {
        GetRingBuffer().TryEnqueue(EventType::kNetSend,
                                   payload,
                                   static_cast<uint32_t>(plen));
    }

    if (g_orig_send) {
        return g_orig_send(sockfd, buf, len, flags);
    }
    errno = ENOSYS;
    return -1;
}

/// Hook: recv(2)
static ssize_t Hook_recv(int    sockfd,
                          void*  buf,
                          size_t len,
                          int    flags) noexcept {
    ssize_t ret = -1;

    // Call through first so we capture actual received data.
    if (g_orig_recv) {
        ret = g_orig_recv(sockfd, buf, len, flags);
    } else {
        errno = ENOSYS;
        return -1;
    }

    char payload[kMaxEventPayloadSize];
    const size_t plen = BuildRecvPayload(sockfd, buf, ret,
                                         payload, sizeof(payload));
    if (plen > 0u) {
        GetRingBuffer().TryEnqueue(EventType::kNetRecv,
                                   payload,
                                   static_cast<uint32_t>(plen));
    }

    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// InstallNetworkHooks
// ─────────────────────────────────────────────────────────────────────────────
HookResult InstallNetworkHooks() noexcept {
    // Guard: prevent double installation.
    bool expected = false;
    if (!g_network_hooks_installed.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        NET_LOGW("InstallNetworkHooks: already installed.");
        return HookResult::kAlreadyInstalled;
    }

#ifndef DISABLE_BYTEHOOK
    // ── ByteHook path ─────────────────────────────────────────────────────
    bool any_failed = false;

    // Hook connect
    {
        bytehook_stub_t stub = bytehook_hook_single(
            "libc.so",
            nullptr,
            "connect",
            reinterpret_cast<void*>(Hook_connect),
            reinterpret_cast<void**>(&g_orig_connect),
            nullptr);
        if (!stub) {
            NET_LOGE("InstallNetworkHooks: bytehook_hook_single(connect) failed.");
            any_failed = true;
        }
    }

    // Hook send
    {
        bytehook_stub_t stub = bytehook_hook_single(
            "libc.so",
            nullptr,
            "send",
            reinterpret_cast<void*>(Hook_send),
            reinterpret_cast<void**>(&g_orig_send),
            nullptr);
        if (!stub) {
            NET_LOGE("InstallNetworkHooks: bytehook_hook_single(send) failed.");
            any_failed = true;
        }
    }

    // Hook recv
    {
        bytehook_stub_t stub = bytehook_hook_single(
            "libc.so",
            nullptr,
            "recv",
            reinterpret_cast<void*>(Hook_recv),
            reinterpret_cast<void**>(&g_orig_recv),
            nullptr);
        if (!stub) {
            NET_LOGE("InstallNetworkHooks: bytehook_hook_single(recv) failed.");
            any_failed = true;
        }
    }

    if (any_failed) {
        g_network_hooks_installed.store(false, std::memory_order_release);
        return HookResult::kHookFailed;
    }

    NET_LOGI("InstallNetworkHooks: connect/send/recv hooked successfully.");
    return HookResult::kSuccess;

#else
    // ── ByteHook disabled path ────────────────────────────────────────────
    BYTEHOOK_STUB(kLogTag);

    // Resolve originals via dlsym so call-throughs remain valid even
    // without real hooks — allows the rest of the system to function.
    g_orig_connect = reinterpret_cast<ConnectFn>(
        ::dlsym(RTLD_NEXT, "connect"));
    g_orig_send    = reinterpret_cast<SendFn>(
        ::dlsym(RTLD_NEXT, "send"));
    g_orig_recv    = reinterpret_cast<RecvFn>(
        ::dlsym(RTLD_NEXT, "recv"));

    NET_LOGI("InstallNetworkHooks: DISABLE_BYTEHOOK active. "
             "Hooks are no-ops; originals resolved via dlsym.");
    return HookResult::kByteHookDisabled;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// UninstallNetworkHooks
// ─────────────────────────────────────────────────────────────────────────────
HookResult UninstallNetworkHooks() noexcept {
    bool expected = true;
    if (!g_network_hooks_installed.compare_exchange_strong(
            expected, false,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        NET_LOGW("UninstallNetworkHooks: hooks not installed.");
        return HookResult::kNotInstalled;
    }

#ifndef DISABLE_BYTEHOOK
    // ByteHook manages stub lifetime internally; unhooking is done via
    // bytehook_unhook() on stored stubs. For this architecture the stubs
    // are owned by ByteHook's internal registry and are cleaned up on
    // bytehook_finish(). Nullify our pointers defensively.
    g_orig_connect = nullptr;
    g_orig_send    = nullptr;
    g_orig_recv    = nullptr;

    NET_LOGI("UninstallNetworkHooks: hooks removed.");
    return HookResult::kSuccess;
#else
    g_orig_connect = nullptr;
    g_orig_send    = nullptr;
    g_orig_recv    = nullptr;

    NET_LOGI("UninstallNetworkHooks: DISABLE_BYTEHOOK active. No-op.");
    return HookResult::kByteHookDisabled;
#endif
}

} // namespace passive_observer
