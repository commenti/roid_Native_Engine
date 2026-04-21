#include "hook_manager.h"
#include "ring_buffer.h"
#include "config.h"

#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <cstdint>

// bhook (ByteHook) header
#include <bytehook.h>

#define LOG_TAG "Observer::Main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

namespace observer {

// ─────────────────────────────────────────────
// Forward declarations for dispatcher lifecycle
// Defined in dispatcher.cpp, declared in
// hook_manager.h - included above
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
// Static destructor guard
// Ensures clean shutdown when .so is unloaded
// ─────────────────────────────────────────────
namespace {
    struct ObserverLifetimeGuard {
        ~ObserverLifetimeGuard() noexcept {
            LOGI("ObserverLifetimeGuard: initiating shutdown.");
            uninstall_all_hooks();
            dispatcher_stop();
            LOGI("ObserverLifetimeGuard: shutdown complete.");
        }
    };
    // One static instance - destructor fires on dlclose / process exit
    static ObserverLifetimeGuard g_lifetime_guard;
} // anonymous namespace

// ─────────────────────────────────────────────
// Core initialisation - called from JNI_OnLoad
// ─────────────────────────────────────────────
static bool initialise() noexcept {
    LOGI("=== Passive Native Observer Initialising ===");
    LOGI("Target server: %s:%u", config::SERVER_IP, config::SERVER_PORT);
    LOGI("Ring buffer size: %zu bytes", config::RING_BUFFER_SIZE);

    // ── Step 1: Initialise bhook ─────────────────────────
    // BYTEHOOK_MODE_AUTOMATIC: bhook selects PLT or inline
    // hooking strategy per target automatically
    int bh_ret = bytehook_init(BYTEHOOK_MODE_AUTOMATIC, false);
    if (bh_ret != BYTEHOOK_STATUS_CODE_OK) {
        LOGE("bytehook_init() failed with code: %d", bh_ret);
        return false;
    }
    LOGI("bhook initialised successfully (mode=AUTOMATIC).");

    // ── Step 2: Start async dispatcher thread ────────────
    // Must start BEFORE installing hooks so the ring buffer
    // consumer is ready before any producer can push data
    dispatcher_start();

    // ── Step 3: Install network hooks ───────────────────
    if (!install_network_hooks()) {
        // Non-fatal: log warning, continue with crypto hooks
        LOGW("Network hooks installation reported failure. "
             "Partial coverage may apply.");
    }

    // ── Step 4: Install crypto / SSL hooks ───────────────
    if (!install_crypto_hooks()) {
        // Non-fatal: app may not use SSL directly
        LOGW("Crypto hooks installation reported failure. "
             "SSL traffic may not be captured.");
    }

    LOGI("=== Passive Native Observer Active ===");
    return true;
}

} // namespace observer

// ─────────────────────────────────────────────
// JNI_OnLoad
// Entry point when System.loadLibrary("observer")
// is called from the injected smali or the app
// ─────────────────────────────────────────────
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;

    if (!observer::initialise()) {
        // Return invalid version to signal load failure
        // The system will unload the library
        __android_log_print(ANDROID_LOG_ERROR,
                            "Observer::Main",
                            "Fatal: initialise() failed. Library will be unloaded.");
        return JNI_ERR;
    }

    // Require JNI version 1.6 minimum (Android 14/15 compatible)
    return JNI_VERSION_1_6;
}

// ─────────────────────────────────────────────
// JNI_OnUnload
// Called when the ClassLoader holding this
// library is GC'd - supplement to static dtor
// ─────────────────────────────────────────────
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;

    __android_log_print(ANDROID_LOG_INFO,
                        "Observer::Main",
                        "JNI_OnUnload called - static destructors will handle cleanup.");
    // ObserverLifetimeGuard destructor handles:
    // 1. uninstall_all_hooks()
    // 2. dispatcher_stop()
    // No explicit action needed here - guard fires after this returns
}