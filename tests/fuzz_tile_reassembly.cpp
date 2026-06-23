#include "tile_reassembly.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

using namespace waydisplay;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static ClientState     state;
    static bool            initialized = false;
    static TileReassembler reassembler(64, 1024u * 1024u);
    if (!initialized)
    {
        state.config.session_id         = 1;
        state.config.connection_token   = 2;
        state.config.content_epoch      = 1;
        state.config.config_epoch       = 1;
        state.config.server_udp_port    = 5001;
        state.config.width              = 256;
        state.config.height             = 256;
        state.config.tile_width         = 16;
        state.config.tile_height        = 16;
        state.config.tiles_x            = 16;
        state.config.tiles_y            = 16;
        state.config.total_tiles        = 256;
        state.config.udp_payload_target = 1200;
        state.received_generation.assign(256, 0);
        state.retx_queued_generation.assign(256, 0);
        state.retx_last_requested_generation.assign(256, 0);
        state.retx_last_request_ns.assign(256, 0);
        state.retx_inflight_generation.assign(256, 0);
        state.retx_inflight_since_ns.assign(256, 0);
        initialized = true;
    }
    CompletedTile completed = reassembler.process_udp_packet(state, data, size);
    if (completed.valid)
    {
        reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));
    }
    return 0;
}
