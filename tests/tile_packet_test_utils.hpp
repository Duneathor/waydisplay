#pragma once

#include "tile_reassembly.hpp"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace waydisplay::test {

constexpr uint16_t kStructuredUdpPayloadTarget = 256;
constexpr uint32_t kStructuredTileBytes = 16u * 16u * WD_BYTES_PER_PIXEL;
constexpr uint8_t  kStructuredFragmentCount =
    static_cast<uint8_t>((kStructuredTileBytes + kStructuredUdpPayloadTarget - 1u) /
                         kStructuredUdpPayloadTarget);

inline void initialize_structured_tile_state(ClientState& state, uint8_t session_id,
                                             uint64_t connection_token,
                                             uint64_t content_epoch,
                                             uint16_t width = 128,
                                             uint16_t height = 128) {
    state.config.session_id         = session_id;
    state.config.connection_token   = connection_token;
    state.config.content_epoch      = content_epoch;
    state.config.config_epoch       = content_epoch;
    state.config.server_udp_port    = 5001;
    state.config.width              = width;
    state.config.height             = height;
    state.config.tile_width         = 16;
    state.config.tile_height        = 16;
    state.config.tiles_x            = wd_tiles_for_width_with_tile(width, 16);
    state.config.tiles_y            = wd_tiles_for_height_with_tile(height, 16);
    state.config.total_tiles        = static_cast<uint16_t>(
        static_cast<uint32_t>(state.config.tiles_x) * static_cast<uint32_t>(state.config.tiles_y));
    state.config.udp_payload_target = kStructuredUdpPayloadTarget;
    state.received_generation.assign(state.config.total_tiles, 0);
    state.retx_queued_generation.assign(state.config.total_tiles, 0);
    state.retx_last_requested_generation.assign(state.config.total_tiles, 0);
    state.retx_last_request_ns.assign(state.config.total_tiles, 0);
    state.retx_inflight_generation.assign(state.config.total_tiles, 0);
    state.retx_inflight_since_ns.assign(state.config.total_tiles, 0);
}

inline std::vector<uint8_t> make_uncompressed_tile_fragment(const ClientState& state,
                                                            uint16_t tile_id,
                                                            uint64_t generation,
                                                            uint8_t fragment_id,
                                                            uint8_t payload_seed,
                                                            uint64_t input_sequence = 0) {
    if (fragment_id >= kStructuredFragmentCount)
    {
        return {};
    }

    const uint32_t offset = static_cast<uint32_t>(fragment_id) * kStructuredUdpPayloadTarget;
    const uint16_t payload_size = static_cast<uint16_t>(
        std::min<uint32_t>(kStructuredUdpPayloadTarget, kStructuredTileBytes - offset));

    wd_udp_tile_packet_decoded header{};
    header.session_id        = state.config.session_id;
    header.connection_token  = state.config.connection_token;
    header.content_epoch     = state.config.content_epoch;
    header.flags             = input_sequence != 0 ? WD_UDP_TILE_FLAG_INPUT_SEQUENCE : 0;
    header.tile_size         = WD_TILE_16x16;
    header.tile_pkt_id       = fragment_id;
    header.tile_id           = tile_id;
    header.tile_pkt_count    = kStructuredFragmentCount;
    header.payload_size      = payload_size;
    header.tile_payload_size = static_cast<uint16_t>(kStructuredTileBytes);
    header.tile_generation   = generation;
    header.input_sequence    = input_sequence;

    const uint16_t header_size = wd_udp_tile_header_size_for_flags(header.flags);
    std::vector<uint8_t> packet(static_cast<size_t>(header_size) + payload_size);
    if (!wd_udp_tile_packet_encode_header(packet.data(), packet.size(), &header))
    {
        return {};
    }

    for (uint16_t i = 0; i < payload_size; ++i)
    {
        packet[static_cast<size_t>(header_size) + i] =
            static_cast<uint8_t>(payload_seed + fragment_id * 17u + static_cast<uint8_t>(i));
    }
    return packet;
}

} // namespace waydisplay::test
