#include "ring_buffer.h"
#include <algorithm>

namespace observer::core {

RingBuffer g_ring_buffer;

bool RingBuffer::push(const void* payload, uint32_t length) {
    if (payload == nullptr || length == 0) {
        return false;
    }

    const uint32_t current_tail = tail_.load(std::memory_order_relaxed);
    const uint32_t next_tail    = current_tail + 1;

    // SPSC capacity check with acquire to see latest consumer progress
    if ((next_tail - head_.load(std::memory_order_acquire)) > config::RB_CAPACITY) {
        return false; // Ring buffer is full
    }

    const uint32_t idx = current_tail & MASK;
    const uint32_t safe_len = std::min(length, static_cast<uint32_t>(config::RB_PAYLOAD_MAX));
    
    // Copy data into pre-allocated slot
    buffer_[idx].length = safe_len;
    std::memcpy(buffer_[idx].data, payload, safe_len);

    // Release ensures data writes are visible before index advances
    tail_.store(next_tail, std::memory_order_release);
    return true;
}

bool RingBuffer::pop(RingBufferEvent& out_event) {
    const uint32_t current_head = head_.load(std::memory_order_relaxed);

    // Check if producer has advanced past us
    if (current_head == tail_.load(std::memory_order_acquire)) {
        return false; // Ring buffer is empty
    }

    const uint32_t idx = current_head & MASK;
    out_event = buffer_[idx];

    head_.store(current_head + 1, std::memory_order_release);
    return true;
}

bool RingBuffer::empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_relaxed);
}

size_t RingBuffer::size() const {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_relaxed);
}

} // namespace observer::core
