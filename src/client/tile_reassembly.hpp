#pragma once

#include <cstdint>
#include <vector>

#include "wd_client.hpp"

namespace waydisplay {

struct CompletedTile {
    bool valid = false;
    uint16_t tile_id = 0;
    uint64_t generation = 0;
    std::vector<uint8_t> tile_bytes;
};

class TileReassembler {
public:
    TileReassembler();

    CompletedTile process_udp_packet(ClientState& state,
                                     const uint8_t* packet,
                                     size_t packet_size);

private:
    struct Entry {
        bool active = false;
        uint16_t tile_id = 0;
        uint64_t generation = 0;
        uint16_t packet_count = 0;
        uint32_t compressed_size = 0;
        uint64_t first_packet_ns = 0;
        std::vector<uint8_t> compressed;
        std::vector<uint8_t> received;
        uint16_t received_count = 0;
    };

    std::vector<Entry> entries_;
};

} // namespace waydisplay
