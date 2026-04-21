#ifndef OBSERVER_CONFIG_H
#define OBSERVER_CONFIG_H

#include <cstdint>
#include <cstddef>

namespace observer::config {
    // Termux TCP Server details
    inline constexpr const char* SERVER_IP = "127.0.0.1";
    inline constexpr uint16_t SERVER_PORT = 8888;

    // SPSC Ring Buffer Size (Must be power of 2 for bitmask indexing)
    inline constexpr size_t RING_BUFFER_SIZE = 2097152; // 2MB
    inline constexpr size_t BUFFER_MASK = RING_BUFFER_SIZE - 1;

    // Data limits
    inline constexpr size_t MAX_PAYLOAD_SIZE = 8192;

    enum class EventType : uint8_t {
        NET_CONNECT = 1,
        NET_SEND    = 2,
        NET_RECV    = 3,
        SSL_WRITE   = 4,
        SSL_READ    = 5
    };

    #pragma pack(push, 1)
    struct LogPacket {
        uint64_t timestamp_ns;
        EventType type;
        uint32_t pid;
        uint32_t tid;
        uint32_t data_len;
        // Followed by raw data_len bytes
    };
    #pragma pack(pop)
}

#endif // OBSERVER_CONFIG_H