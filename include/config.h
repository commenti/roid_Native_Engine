#pragma once

#include <android/log.h>
#include <cstdint>

namespace observer::config {
    // Lock-Free SPSC Ring Buffer Configuration
    // Pre-allocated in .bss/data section to guarantee zero-malloc during hooks
    constexpr uint32_t RB_PAYLOAD_MAX = 256;
    constexpr uint32_t RB_CAPACITY    = 1024;
    constexpr uint32_t RB_TOTAL_SIZE  = RB_PAYLOAD_MAX * RB_CAPACITY;

    // Async Dispatcher TCP Configuration
    constexpr const char* SERVER_HOST = "127.0.0.1";
    constexpr uint16_t    SERVER_PORT = 41956;
    constexpr int         TCP_TIMEOUT_MS = 50;

    // Diagnostic
    constexpr const char* LOG_TAG = "PassiveObserver";
}

// Strict stack-only logging macro. Bypasses std::string/iostream to prevent heap allocation.
#define OBS_LOG(prio, fmt, ...) \
    __android_log_print(prio, observer::config::LOG_TAG, fmt, ##__VA_ARGS__)
