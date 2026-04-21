#ifndef OBSERVER_HOOK_MANAGER_H
#define OBSERVER_HOOK_MANAGER_H

namespace observer {

// ─────────────────────────────────────────────
// Public API - called from JNI_OnLoad (main.cpp)
// ─────────────────────────────────────────────

// Install all network hooks (connect, send, recv)
// Returns true if all hooks installed successfully
bool install_network_hooks() noexcept;

// Install all crypto hooks (SSL_write, SSL_read)
// Returns true if all hooks installed successfully
bool install_crypto_hooks() noexcept;

// Remove all installed hooks cleanly
void uninstall_all_hooks() noexcept;

// ─────────────────────────────────────────────
// Dispatcher lifecycle - declared here so
// main.cpp only needs hook_manager.h
// ─────────────────────────────────────────────
void dispatcher_start() noexcept;
void dispatcher_stop()  noexcept;

} // namespace observer

#endif // OBSERVER_HOOK_MANAGER_H