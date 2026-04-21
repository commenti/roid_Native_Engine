#include "hook_manager.h"
#include "ring_buffer.h"
#include "config.h"

#include <cstring>
#include <cstdint>
#include <cstdio>

#include <android/log.h>

// bhook (ByteHook) header
#include <bytehook.h>

#define LOG_TAG "Observer::CryptoHooks"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace observer {

// ─────────────────────────────────────────────
// Forward declaration of uninstall helper
// defined in hooks_network.cpp - we extend it
// here by storing crypto stubs separately and
// calling bytehook_unhook in uninstall_all_hooks
// NOTE: uninstall_all_hooks() is DEFINED in
// hooks_network.cpp but we ADD crypto stubs
// via a dedicated internal cleanup registered
// through a static destructor below.
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
// bhook stub handles
// ─────────────────────────────────────────────
static bytehook_stub_t g_stub_ssl_write = nullptr;
static bytehook_stub_t g_stub_ssl_read  = nullptr;

// ─────────────────────────────────────────────
// SSL opaque pointer type alias
// We do NOT include openssl headers to avoid
// dependency issues - SSL* is treated as void*
// ─────────────────────────────────────────────
using SSL_ptr = void*;

// Original function typedefs matching OpenSSL ABI
using ssl_write_fn_t = int(*)(SSL_ptr, const void*, int);
using ssl_read_fn_t  = int(*)(SSL_ptr, void*, int);

// ─────────────────────────────────────────────
// Stack-allocated serialisation helper
// Produces JSON with hex-encoded TLS payload
// NO malloc, NO heap
// ─────────────────────────────────────────────
static uint32_t serialise_ssl(
    uint8_t*       out,
    uint32_t       out_size,
    const char*    event_name,
    SSL_ptr        ssl,
    const uint8_t* data,
    int            data_len) noexcept
{
    if (data_len <= 0 || !data) {
        return 0;
    }

    // Hex-encode up to 128 bytes of plaintext TLS payload
    constexpr size_t MAX_HEX_BYTES = 128;
    const size_t hex_bytes = (static_cast<size_t>(data_len) < MAX_HEX_BYTES)
                             ? static_cast<size_t>(data_len)
                             : MAX_HEX_BYTES;

    // 2 hex chars per byte + null terminator
    char hex_buf[MAX_HEX_BYTES * 2 + 1] = {};
    static constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < hex_bytes; ++i) {
        hex_buf[i * 2 + 0] = HEX[(data[i] >> 4) & 0xF];
        hex_buf[i * 2 + 1] = HEX[(data[i])      & 0xF];
    }

    // ssl pointer used as opaque session identifier
    int written = ::snprintf(
        reinterpret_cast<char*>(out), out_size,
        R"({"event":"%s","ssl_ctx":"0x%016llx","len":%d,"hex":"%s","truncated":%s})",
        event_name,
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(ssl)),
        data_len,
        hex_buf,
        (static_cast<size_t>(data_len) > MAX_HEX_BYTES) ? "true" : "false");

    if (written < 0 || static_cast<uint32_t>(written) >= out_size) {
        return 0;
    }

    return static_cast<uint32_t>(written);
}

// ─────────────────────────────────────────────
// Hook: SSL_write
// Captures plaintext BEFORE encryption
// STRICT: NO malloc, NO I/O, NO blocking
// ─────────────────────────────────────────────
static int hook_ssl_write(SSL_ptr ssl,
                          const void* buf,
                          int num) noexcept {
    BYTEHOOK_STACK_SCOPE();

    // Call original FIRST - capture return to verify success
    int ret = BYTEHOOK_CALL_PREV(hook_ssl_write, ssl_write_fn_t,
                                 ssl, buf, num);

    // Only log if bytes were actually written
    if (ret > 0 && buf) {
        uint8_t serial_buf[640]; // stack allocated
        uint32_t serial_len = serialise_ssl(
            serial_buf,
            sizeof(serial_buf),
            "SSL_WRITE",
            ssl,
            reinterpret_cast<const uint8_t*>(buf),
            ret);

        if (serial_len > 0) {
            get_ring_buffer().push(
                config::EventType::SSL_WRITE,
                serial_buf,
                serial_len);
        }
    }

    return ret;
}

// ─────────────────────────────────────────────
// Hook: SSL_read
// Captures plaintext AFTER decryption
// STRICT: NO malloc, NO I/O, NO blocking
// ─────────────────────────────────────────────
static int hook_ssl_read(SSL_ptr ssl,
                         void* buf,
                         int num) noexcept {
    BYTEHOOK_STACK_SCOPE();

    // Call original FIRST - buf is populated after this
    int ret = BYTEHOOK_CALL_PREV(hook_ssl_read, ssl_read_fn_t,
                                 ssl, buf, num);

    // Only log if bytes were actually read into buf
    if (ret > 0 && buf) {
        uint8_t serial_buf[640]; // stack allocated
        uint32_t serial_len = serialise_ssl(
            serial_buf,
            sizeof(serial_buf),
            "SSL_READ",
            ssl,
            reinterpret_cast<const uint8_t*>(buf),
            ret);

        if (serial_len > 0) {
            get_ring_buffer().push(
                config::EventType::SSL_READ,
                serial_buf,
                serial_len);
        }
    }

    return ret;
}

// ─────────────────────────────────────────────
// Static destructor: cleans up crypto stubs
// Runs before .so is unloaded, supplements
// uninstall_all_hooks() from hooks_network.cpp
// ─────────────────────────────────────────────
namespace {
    struct CryptoHookGuard {
        ~CryptoHookGuard() noexcept {
            if (g_stub_ssl_write) {
                bytehook_unhook(g_stub_ssl_write);
                g_stub_ssl_write = nullptr;
            }
            if (g_stub_ssl_read) {
                bytehook_unhook(g_stub_ssl_read);
                g_stub_ssl_read = nullptr;
            }
        }
    };
    // One static instance per .so lifetime
    static CryptoHookGuard g_crypto_guard;
} // anonymous namespace

// ─────────────────────────────────────────────
// Public: install_crypto_hooks()
// Targets libssl.so which ships with most apps
// Also targets libconscrypt_jni.so (Android TLS)
// ─────────────────────────────────────────────
bool install_crypto_hooks() noexcept {
    // Target libssl.so - standard OpenSSL / BoringSSL
    g_stub_ssl_write = bytehook_hook_all(
        nullptr,                 // all callers
        "SSL_write",
        reinterpret_cast<void*>(hook_ssl_write),
        nullptr,
        nullptr);

    g_stub_ssl_read = bytehook_hook_all(
        nullptr,
        "SSL_read",
        reinterpret_cast<void*>(hook_ssl_read),
        nullptr,
        nullptr);

    if (!g_stub_ssl_write || !g_stub_ssl_read) {
        LOGE("One or more SSL hooks failed to install. "
             "SSL_write=%p SSL_read=%p",
             g_stub_ssl_write, g_stub_ssl_read);
        // Non-fatal: app may not use libssl directly
        // conscrypt / okhttp may still be intercepted
        // via send/recv hooks from hooks_network.cpp
        return false;
    }

    LOGI("Crypto hooks installed: SSL_write=%p SSL_read=%p",
         g_stub_ssl_write, g_stub_ssl_read);
    return true;
}

} // namespace observer