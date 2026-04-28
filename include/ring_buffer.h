#pragma once
#include "config.h"
#include <atomic>
#include <cstdint>
#include <cstring>

namespace observer::core {

struct RingBufferEvent {
    uint8_t  data[config::RB_PAYLOAD_MAX];
    uint32_t length = 0;
};

class RingBuffer {
public:
    RingBuffer() = default;

    // Producer interface (Hook context)
    bool push(const void* payload, uint32_t length);

    // Consumer interface (Dispatcher thread)
    bool pop(RingBufferEvent& out_event);

    bool empty() const;
    size_t size() const;

private:
    RingBufferEvent buffer_[config::RB_CAPACITY]{};
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    static constexpr uint32_t MASK = config::RB_CAPACITY - 1;
};

// Static allocation in .bss to guarantee zero-malloc in hook paths
extern RingBuffer g_ring_buffer;

} // namespace observer::core
