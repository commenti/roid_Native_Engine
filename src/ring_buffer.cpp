#include "ring_buffer.h"
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <android/log.h>

#define LOG_TAG "Observer::RingBuffer"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace observer {

// ─────────────────────────────────────────────
// Static singleton storage - zero heap usage
// ─────────────────────────────────────────────
static RingBuffer g_ring_buffer;

RingBuffer& get_ring_buffer() noexcept {
    return g_ring_buffer;
}

// ─────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────
RingBuffer::RingBuffer()
    : write_idx_(0),
      read_idx_(0) {
    ::memset(buffer_, 0, sizeof(buffer_));
}

// ─────────────────────────────────────────────
// Internal: write bytes into circular buffer
// ─────────────────────────────────────────────
void RingBuffer::write_bytes(const uint8_t* src, size_t len, size_t start_idx) noexcept {
    const size_t space_to_end = config::RING_BUFFER_SIZE - (start_idx & config::BUFFER_MASK);
    if (len <= space_to_end) {
        ::memcpy(buffer_ + (start_idx & config::BUFFER_MASK), src, len);
    } else {
        // Wrap around
        ::memcpy(buffer_ + (start_idx & config::BUFFER_MASK), src, space_to_end);
        ::memcpy(buffer_, src + space_to_end, len - space_to_end);
    }
}

// ─────────────────────────────────────────────
// Internal: read bytes from circular buffer
// ─────────────────────────────────────────────
void RingBuffer::read_bytes(uint8_t* dst, size_t len, size_t start_idx) const noexcept {
    const size_t space_to_end = config::RING_BUFFER_SIZE - (start_idx & config::BUFFER_MASK);
    if (len <= space_to_end) {
        ::memcpy(dst, buffer_ + (start_idx & config::BUFFER_MASK), len);
    } else {
        ::memcpy(dst, buffer_ + (start_idx & config::BUFFER_MASK), space_to_end);
        ::memcpy(dst + space_to_end, buffer_, len - space_to_end);
    }
}

// ─────────────────────────────────────────────
// push() - Producer side (hook threads)
// NO malloc, NO I/O, NO blocking
// ─────────────────────────────────────────────
bool RingBuffer::push(config::EventType type,
                      const uint8_t* payload,
                      uint32_t payload_len) noexcept {
    // Clamp payload to maximum allowed
    if (payload_len > config::MAX_PAYLOAD_SIZE) {
        payload_len = static_cast<uint32_t>(config::MAX_PAYLOAD_SIZE);
    }

    const size_t frame_size = sizeof(config::LogPacket) + payload_len;

    // Snapshot indices with acquire/relaxed semantics
    const size_t cur_write = write_idx_.load(std::memory_order_relaxed);
    const size_t cur_read  = read_idx_.load(std::memory_order_acquire);

    // Check available space (leave 1 frame gap to distinguish full vs empty)
    const size_t used = cur_write - cur_read;
    if ((config::RING_BUFFER_SIZE - used) < frame_size) {
        // Buffer full - drop event, safe in hook context
        return false;
    }

    // Build header on stack - zero heap usage
    config::LogPacket hdr{};

    // Timestamp via clock_gettime (async-signal-safe)
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    hdr.timestamp_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                       + static_cast<uint64_t>(ts.tv_nsec);

    hdr.type     = type;
    hdr.pid      = static_cast<uint32_t>(::getpid());
    hdr.tid      = static_cast<uint32_t>(::gettid());
    hdr.data_len = payload_len;

    // Write header then payload contiguously
    write_bytes(reinterpret_cast<const uint8_t*>(&hdr),
                sizeof(config::LogPacket),
                cur_write);

    write_bytes(payload,
                payload_len,
                cur_write + sizeof(config::LogPacket));

    // Publish: release so consumer sees completed writes
    write_idx_.store(cur_write + frame_size, std::memory_order_release);

    return true;
}

// ─────────────────────────────────────────────
// pop() - Consumer side (dispatcher thread)
// ─────────────────────────────────────────────
size_t RingBuffer::pop(uint8_t* out_buf, size_t out_buf_len) noexcept {
    const size_t cur_read  = read_idx_.load(std::memory_order_relaxed);
    const size_t cur_write = write_idx_.load(std::memory_order_acquire);

    if (cur_read == cur_write) {
        // Buffer empty
        return 0;
    }

    // Read header first to learn frame size
    if (out_buf_len < sizeof(config::LogPacket)) {
        return 0;
    }

    config::LogPacket hdr{};
    read_bytes(reinterpret_cast<uint8_t*>(&hdr),
               sizeof(config::LogPacket),
               cur_read);

    const size_t frame_size = sizeof(config::LogPacket) + hdr.data_len;

    // Validate frame is fully written and fits output buffer
    const size_t available = cur_write - cur_read;
    if (available < frame_size || out_buf_len < frame_size) {
        return 0;
    }

    // Copy full frame (header + payload) into output buffer
    read_bytes(out_buf, frame_size, cur_read);

    // Advance read index - release so producer sees freed space
    read_idx_.store(cur_read + frame_size, std::memory_order_release);

    return frame_size;
}

// ─────────────────────────────────────────────
// available_read() - consumer-side hint only
// ─────────────────────────────────────────────
size_t RingBuffer::available_read() const noexcept {
    const size_t w = write_idx_.load(std::memory_order_acquire);
    const size_t r = read_idx_.load(std::memory_order_relaxed);
    return w - r;
}

} // namespace observer