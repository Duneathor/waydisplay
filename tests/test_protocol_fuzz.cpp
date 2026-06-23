#include "tile_reassembly.hpp"
#include "waydisplay/wd_protocol.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void initialize_state(ClientState& state) {
    state.config.session_id         = 7;
    state.config.connection_token   = 0x1111222233334444ull;
    state.config.content_epoch      = 2;
    state.config.config_epoch       = 2;
    state.config.server_udp_port    = 5001;
    state.config.width              = 128;
    state.config.height             = 128;
    state.config.tile_width         = 16;
    state.config.tile_height        = 16;
    state.config.tiles_x            = 8;
    state.config.tiles_y            = 8;
    state.config.total_tiles        = 64;
    state.config.udp_payload_target = 1200;
    state.received_generation.assign(64, 0);
    state.retx_queued_generation.assign(64, 0);
    state.retx_last_requested_generation.assign(64, 0);
    state.retx_last_request_ns.assign(64, 0);
    state.retx_inflight_generation.assign(64, 0);
    state.retx_inflight_since_ns.assign(64, 0);
}

void test_random_protocol_inputs_are_bounded() {
    std::mt19937_64 random(0x5eed1234ull);
    ClientState     state;
    initialize_state(state);
    TileReassembler reassembler(32, 256u * 1024u);

    for (size_t iteration = 0; iteration < 20000; ++iteration)
    {
        const size_t         size = static_cast<size_t>(random() % 2049u);
        std::vector<uint8_t> bytes(size);
        for (uint8_t& byte : bytes)
        {
            byte = static_cast<uint8_t>(random());
        }

        wd_udp_tile_packet_decoded decoded{};
        const bool                 valid = wd_udp_tile_packet_decode(bytes.data(), bytes.size(), &decoded);
        if (valid)
        {
            require(decoded.header_size <= bytes.size(), "decoded header must fit packet");
            require(decoded.payload_size + decoded.header_size == bytes.size(), "valid packets are canonical");
        }

        CompletedTile completed = reassembler.process_udp_packet(state, bytes.data(), bytes.size());
        if (completed.valid)
        {
            require(completed.tile_bytes.size() <= WD_TCP_MAX_PAYLOAD_SIZE, "fuzz completion must remain bounded");
            reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));
        }
        require(reassembler.active_entry_count() <= 32, "fuzz reassembly entry budget");
        require(reassembler.active_payload_bytes() <= 256u * 1024u, "fuzz reassembly byte budget");
    }
}

} // namespace

int main() {
    test_random_protocol_inputs_are_bounded();
    return 0;
}
