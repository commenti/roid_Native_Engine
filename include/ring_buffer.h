#ifndef OBSERVER_RING_BUFFER_H
#define OBSERVER_RING_BUFFER_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "config.h"

namespace observer {

// SPSC Lock-Free Ring Buffer
// Producer: hook threads (NO malloc, NO I/O)
// Consumer: dispatcher thread
class RingBuffer {
public:
    RingBuffer();
    ~RingBuffer() = default;

    // Called ONLY from producer (hook) side
    // Writes a LogPacket header + raw payload atomically into the buffer
    // Returns false if not enough space (drop event)
    bool push(config::EventType type,
              const uint8_t* payload,
              uint32_t payload_len) noexcept;

    // Called ONLY from consumer (dispatcher) side
    // Copies next available frame into out_buf (must be MAX_PAYLOAD_SIZE + sizeof(LogPacket) large)
    // Returns number of bytes written into out_buf, or 0 if empty
    size_t pop(uint8_t* out_buf, size_t out_buf_len) noexcept;

    // Returns approximate bytes available for reading (consumer side only)
    size_t available_read() const noexcept;

private:
    // Raw byte storage - statically allocated, no heap
    alignas(64) uint8_t buffer_[config::RING_BUFFER_SIZE];

    // Write index: owned by producer
    alignas(64) std::atomic<size_t> write_idx_;

    // Read index: owned by consumer
    alignas(64) std::atomic<size_t> read_idx_;

    // Writes `len` bytes from `src` into circular buffer at current write_idx_
    // UNSAFE: caller must guarantee space exists
    void write_bytes(const uint8_t* src, size_t len, size_t start_idx) noexcept;

    // Reads `len` bytes from circular buffer at current read_idx_ into `dst`
    // UNSAFE: caller must guarantee data exists
    void read_bytes(uint8_t* dst, size_t len, size_t start_idx) const noexcept;
};

// Singleton accessor - initialized once in JNI_OnLoad
RingBuffer& get_ring_buffer() noexcept;

} // namespace observer

#endif // OBSERVER_RING_BUFFER_H