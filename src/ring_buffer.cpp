#include "ring_buffer.h"

#include <cstring>
#include <algorithm>

// Android logcat — used only in diagnostics, never in hot path.
#include <android/log.h>
#include "config.h"

namespace passive_observer {

// ─────────────────────────────────────────────────────────────────────────────
// SPSCRingBuffer — implementation
// ─────────────────────────────────────────────────────────────────────────────

SPSCRingBuffer::SPSCRingBuffer() noexcept {
    // Zero-initialise all slots explicitly to guarantee a clean state
    // regardless of BSS initialisation order.
    for (auto& slot : slots_) {
        slot.payload_len = 0u;
        slot.event_type  = EventType::kUnknown;
        slot.reserved[0] = 0u;
        slot.reserved[1] = 0u;
        std::memset(slot.payload, 0, kMaxEventPayloadSize);
    }

    // Ensure atomic indices start at zero with sequential consistency
    // so the consumer sees a fully-constructed buffer.
    head_.store(0u, std::memory_order_seq_cst);
    tail_.store(0u, std::memory_order_seq_cst);
    dropped_.store(0u, std::memory_order_seq_cst);
}

// ─────────────────────────────────────────────────────────────────────────────
// TryEnqueue  (producer — hook-site context)
//
// Memory ordering rationale:
//   1. Load tail_ with acquire  → synchronises with consumer's release store.
//   2. Store to slot fields     → plain writes; slot is exclusively ours until
//                                 we publish head_.
//   3. Store head_ with release → makes slot data visible to consumer.
// ─────────────────────────────────────────────────────────────────────────────
bool SPSCRingBuffer::TryEnqueue(EventType   type,
                                const void* data,
                                uint32_t    len) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);

    // Full check: one slot is always kept empty as sentinel.
    if ((head - tail) >= kRingBufferCapacity) {
        dropped_.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }

    RingSlot& slot = slots_[Mask(head)];

    // Clamp payload to maximum safe size — never overflow the fixed array.
    const uint32_t copy_len =
        std::min(len, static_cast<uint32_t>(kMaxEventPayloadSize));

    slot.event_type  = type;
    slot.payload_len = copy_len;
    slot.reserved[0] = 0u;
    slot.reserved[1] = 0u;

    if (data != nullptr && copy_len > 0u) {
        std::memcpy(slot.payload, data, copy_len);
    }

    // Publish the slot — release store pairs with consumer's acquire load.
    head_.store(head + 1u, std::memory_order_release);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TryDequeue  (consumer — dispatcher thread)
//
// Memory ordering rationale:
//   1. Load head_ with acquire  → synchronises with producer's release store.
//   2. Read slot fields         → safe; producer won't touch this slot until
//                                 we advance tail_.
//   3. Store tail_ with release → allows producer to reclaim the slot.
// ─────────────────────────────────────────────────────────────────────────────
bool SPSCRingBuffer::TryDequeue(RingSlot& out_slot) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);

    if (tail == head) {
        // Buffer is empty.
        return false;
    }

    const RingSlot& slot = slots_[Mask(tail)];

    // Copy the entire slot to caller-supplied storage.
    out_slot.event_type  = slot.event_type;
    out_slot.payload_len = slot.payload_len;
    out_slot.reserved[0] = slot.reserved[0];
    out_slot.reserved[1] = slot.reserved[1];

    if (slot.payload_len > 0u) {
        std::memcpy(out_slot.payload, slot.payload, slot.payload_len);
    }

    // Release the slot back to the producer.
    tail_.store(tail + 1u, std::memory_order_release);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────
size_t SPSCRingBuffer::ApproxSize() const noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    return (head >= tail) ? (head - tail) : 0u;
}

bool SPSCRingBuffer::IsEmpty() const noexcept {
    return head_.load(std::memory_order_relaxed) ==
           tail_.load(std::memory_order_relaxed);
}

uint64_t SPSCRingBuffer::DroppedCount() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Global singleton
//
// Constructed once during .init_array (before JNI_OnLoad).
// The object lives for the entire process lifetime — no destructor races.
// ─────────────────────────────────────────────────────────────────────────────
static SPSCRingBuffer g_ring_buffer;

SPSCRingBuffer& GetRingBuffer() noexcept {
    return g_ring_buffer;
}

} // namespace passive_observer
