#pragma once

#include <cstddef>
#include <cstdint>

namespace waydisplay {

struct ClientReceiveStatsBatch {
    uint64_t udp_packets_rx                  = 0;
    uint64_t udp_bytes_rx                    = 0;
    uint64_t udp_interarrival_samples        = 0;
    uint64_t udp_interarrival_sum_ns         = 0;
    uint64_t udp_interarrival_jitter_samples = 0;
    uint64_t udp_interarrival_jitter_sum_ns  = 0;
    uint64_t tile_assembly_samples           = 0;
    uint64_t tile_assembly_sum_ns            = 0;
    uint64_t udp_completed_compressed_bytes  = 0;
    uint64_t udp_completed_packets           = 0;
    uint64_t udp_tiles_completed             = 0;

    void note_udp_packet(std::size_t packet_size) noexcept
    {
        ++udp_packets_rx;
        udp_bytes_rx += static_cast<uint64_t>(packet_size);
    }

    void note_interarrival(uint64_t interarrival_ns) noexcept
    {
        ++udp_interarrival_samples;
        udp_interarrival_sum_ns += interarrival_ns;
    }

    void note_jitter(uint64_t jitter_ns) noexcept
    {
        ++udp_interarrival_jitter_samples;
        udp_interarrival_jitter_sum_ns += jitter_ns;
    }

    void note_completed_tile(uint64_t compressed_bytes, uint64_t packet_count, bool has_assembly_sample,
                             uint64_t assembly_ns) noexcept
    {
        ++udp_tiles_completed;
        udp_completed_compressed_bytes += compressed_bytes;
        udp_completed_packets += packet_count;
        if (has_assembly_sample)
        {
            ++tile_assembly_samples;
            tile_assembly_sum_ns += assembly_ns;
        }
    }
};

} // namespace waydisplay
