#pragma once

#ifndef PASSIVE_OBSERVER_HOOK_MANAGER_H
#define PASSIVE_OBSERVER_HOOK_MANAGER_H

#include <cstdint>
#include <cstddef>
#include <sys/types.h>
#include <sys/socket.h>

// ─────────────────────────────────────────────────────────────────────────────
// ByteHook conditional include
// ─────────────────────────────────────────────────────────────────────────────
#ifndef DISABLE_BYTEHOOK
#   include <bytehook.h>
#else
#   include <android/log.h>
#   define BYTEHOOK_STUB(tag) \
        __android_log_print(ANDROID_LOG_WARN, tag, \
            "ByteHook disabled at compile time. Hook is a no-op.")
#endif

#include "config.h"
#include "ring_buffer.h"

// ─────────────────────────────────────────────────────────────────────────────
// DESIGN NOTES
//
// hook_manager.h declares:
//   1. The unified HookResult status enum.
//   2. The public Install / Uninstall surface for each hook group.
//   3. The original-function pointer typedefs that hook implementations
//      use to call through to the real syscall/library function.
//   4. Extern declarations for those original-function pointers so that
//      hooks_network.cpp and hooks_crypto.cpp can share them cleanly.
//
// All hook trampoline implementations live in their respective .cpp files.
// This header is the single contract between main.cpp and the hook modules.
// ─────────────────────────────────────────────────────────────────────────────

namespace passive_observer {

// ─────────────────────────────────────────────────────────────────────────────
// HookResult — unified return code for Install/Uninstall operations
// ─────────────────────────────────────────────────────────────────────────────
enum class HookResult : uint8_t {
    kSuccess         = 0,  ///< Hook installed / uninstalled successfully.
    kByteHookDisabled = 1, ///< Compiled with DISABLE_BYTEHOOK; no-op.
    kAlreadyInstalled = 2, ///< Install called when already active.
    kNotInstalled     = 3, ///< Uninstall called when not active.
    kInitFailed       = 4, ///< ByteHook runtime init failure.
    kHookFailed       = 5, ///< One or more individual hook calls failed.
};

// ─────────────────────────────────────────────────────────────────────────────
// Original-function pointer typedefs
//
// These are the signatures of the real libc / libssl functions we intercept.
// Storing them as typed function pointers (rather than void*) gives us
// type-safe call-throughs inside each hook trampoline.
// ─────────────────────────────────────────────────────────────────────────────

/// connect(2)
using ConnectFn = int (*)(int sockfd,
                           const struct sockaddr* addr,
                           socklen_t addrlen);

/// send(2)
using SendFn = ssize_t (*)(int sockfd,
                            const void* buf,
                            size_t len,
                            int flags);

/// recv(2)
using RecvFn = ssize_t (*)(int sockfd,
                            void*  buf,
                            size_t len,
                            int    flags);

/// SSL_write  (OpenSSL / BoringSSL ABI-compatible signature)
using SslWriteFn = int (*)(void* ssl,
                            const void* buf,
                            int num);

/// SSL_read   (OpenSSL / BoringSSL ABI-compatible signature)
using SslReadFn  = int (*)(void* ssl,
                            void* buf,
                            int   num);

// ─────────────────────────────────────────────────────────────────────────────
// Shared original-function pointer storage
//
// Defined once in hooks_network.cpp / hooks_crypto.cpp.
// Declared extern here so main.cpp can inspect them for diagnostics.
// ─────────────────────────────────────────────────────────────────────────────
extern ConnectFn  g_orig_connect;
extern SendFn     g_orig_send;
extern RecvFn     g_orig_recv;
extern SslWriteFn g_orig_ssl_write;
extern SslReadFn  g_orig_ssl_read;

// ─────────────────────────────────────────────────────────────────────────────
// Network hook group — connect / send / recv
// ─────────────────────────────────────────────────────────────────────────────

/// Installs PLT hooks for connect(2), send(2), recv(2).
///
/// Thread-safe: guarded internally by an atomic flag.
/// Must be called after the ring buffer and dispatcher are initialised.
///
/// @return HookResult::kSuccess            on full success.
///         HookResult::kByteHookDisabled   when compiled with DISABLE_BYTEHOOK.
///         HookResult::kAlreadyInstalled   if called twice.
///         HookResult::kHookFailed         if any individual hook fails.
[[nodiscard]]
HookResult InstallNetworkHooks() noexcept;

/// Removes PLT hooks installed by InstallNetworkHooks().
/// Restores original function pointers.
///
/// @return HookResult::kSuccess          on success.
///         HookResult::kByteHookDisabled when compiled with DISABLE_BYTEHOOK.
///         HookResult::kNotInstalled     if hooks were never installed.
[[nodiscard]]
HookResult UninstallNetworkHooks() noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Crypto hook group — SSL_write / SSL_read
// ─────────────────────────────────────────────────────────────────────────────

/// Installs PLT hooks for SSL_write and SSL_read.
///
/// Targets libssl.so (BoringSSL on Android).
/// Must be called after the ring buffer and dispatcher are initialised.
///
/// @return Same HookResult semantics as InstallNetworkHooks().
[[nodiscard]]
HookResult InstallCryptoHooks() noexcept;

/// Removes PLT hooks installed by InstallCryptoHooks().
///
/// @return Same HookResult semantics as UninstallNetworkHooks().
[[nodiscard]]
HookResult UninstallCryptoHooks() noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Convenience: install / uninstall all hook groups in one call
// ─────────────────────────────────────────────────────────────────────────────

/// Calls InstallNetworkHooks() then InstallCryptoHooks().
/// Returns the first non-success result encountered, or kSuccess if all pass.
[[nodiscard]]
HookResult InstallAllHooks() noexcept;

/// Calls UninstallNetworkHooks() then UninstallCryptoHooks().
/// Continues past individual failures; returns last non-success or kSuccess.
HookResult UninstallAllHooks() noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Utility: human-readable string for a HookResult value
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]]
const char* HookResultToString(HookResult result) noexcept;

} // namespace passive_observer

#endif // PASSIVE_OBSERVER_HOOK_MANAGER_H
