#include "tile_reassembly.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_time.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
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

void initialize_state(ClientState& state, uint16_t udp_payload_target = 400) {
    state.config.session_id = 7;
    state.config.connection_token = 0x123456789abcdef0ull;
    state.config.content_epoch = 5;
    state.config.config_epoch = 3;
    state.config.server_udp_port = 5001;
    state.config.width = 64;
    state.config.height = 64;
    state.config.tile_width = 16;
    state.config.tile_height = 16;
    state.config.tiles_x = 4;
    state.config.tiles_y = 4;
    state.config.total_tiles = 16;
    state.config.udp_payload_target = udp_payload_target;
    state.received_generation.assign(16, 0);
    state.retx_queued_generation.assign(16, 0);
    state.retx_last_requested_generation.assign(16, 0);
    state.retx_last_request_ns.assign(16, 0);
    state.retx_inflight_generation.assign(16, 0);
    state.retx_inflight_since_ns.assign(16, 0);
}

std::vector<std::vector<uint8_t>> make_uncompressed_packets(const ClientState& state,
                                                             uint16_t tile_id,
                                                             uint64_t generation) {
    std::vector<uint8_t> payload(16u * 16u * WD_BYTES_PER_PIXEL);
    for (size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>((i * 17u + tile_id) & 0xffu);
    }

    const uint16_t target = state.config.udp_payload_target;
    const uint8_t count = static_cast<uint8_t>((payload.size() + target - 1u) / target);
    std::vector<std::vector<uint8_t>> packets;
    for (uint8_t packet_id = 0; packet_id < count; ++packet_id)
    {
        const size_t offset = static_cast<size_t>(packet_id) * target;
        const uint16_t size = static_cast<uint16_t>(std::min<size_t>(target, payload.size() - offset));
        wd_udp_tile_packet_decoded header{};
        header.session_id = state.config.session_id;
        header.connection_token = state.config.connection_token;
        header.content_epoch = state.config.content_epoch;
        header.tile_size = WD_TILE_16x16;
        header.tile_pkt_id = packet_id;
        header.tile_id = tile_id;
        header.tile_pkt_count = count;
        header.payload_size = size;
        header.tile_payload_size = static_cast<uint16_t>(payload.size());
        header.tile_generation = generation;
        std::vector<uint8_t> packet(WD_UDP_TILE_HEADER_MIN_SIZE + size);
        require(wd_udp_tile_packet_encode_header(packet.data(), packet.size(), &header),
                "edge-test packet header should encode");
        std::memcpy(packet.data() + WD_UDP_TILE_HEADER_MIN_SIZE, payload.data() + offset, size);
        packets.push_back(std::move(packet));
    }
    return packets;
}

void test_short_header_identity_geometry_and_fragment_counters() {
    ClientState state;
    initialize_state(state);
    TileReassembler reassembler;

    require(!reassembler.process_udp_packet(state, nullptr, 0).valid,
            "null datagram should be rejected");
    require(state.stats.udp_invalid_short.load() == 1 && state.stats.udp_ignored_invalid.load() == 1,
            "short datagram should increment short and invalid counters");

    std::vector<uint8_t> malformed(WD_UDP_TILE_HEADER_MIN_SIZE, 0);
    require(!reassembler.process_udp_packet(state, malformed.data(), malformed.size()).valid,
            "malformed header should be rejected");
    require(state.stats.udp_invalid_header.load() == 1,
            "header decoder rejection should have a distinct counter");

    auto packets = make_uncompressed_packets(state, 0, 1);
    wd_udp_tile_packet_decoded header{};
    require(wd_udp_tile_packet_decode(packets[0].data(), packets[0].size(), &header),
            "canonical edge-test header should decode");

    wd_udp_tile_packet_decoded changed = header;
    changed.session_id++;
    require(validate_tile_packet_header(changed, packets[0].size(), state.config.udp_payload_target, state.config) ==
                TilePacketValidationResult::Identity,
            "session mismatch should be classified as identity");
    changed = header;
    changed.tile_id = 16;
    require(validate_tile_packet_header(changed, packets[0].size(), state.config.udp_payload_target, state.config) ==
                TilePacketValidationResult::Geometry,
            "out-of-grid tile should be classified as geometry");
    changed = header;
    changed.tile_pkt_count++;
    require(validate_tile_packet_header(changed, packets[0].size(), state.config.udp_payload_target, state.config) ==
                TilePacketValidationResult::Fragment,
            "noncanonical packet count should be classified as fragment");
}

void test_payload_larger_than_budget_drops_without_entry() {
    ClientState state;
    initialize_state(state, 1400);
    auto packets = make_uncompressed_packets(state, 0, 2);
    require(packets.size() == 1, "large UDP target should make one tile packet");

    TileReassembler reassembler(4, 100);
    require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
            "tile larger than the reassembly byte budget should be dropped");
    require(state.stats.reassembly_budget_drops.load() == 1,
            "oversized tile should increment the budget-drop counter");
    require(reassembler.active_entry_count() == 0 && reassembler.active_payload_bytes() == 0,
            "oversized tile must not leave partial state");
}

void test_partial_timeout_reduces_adaptive_deadline_and_queues_repair() {
    ClientState state;
    initialize_state(state, 400);
    state.tile_reassembly_floor_ns.store(WD_LINK_TILE_REASSEMBLY_MIN_NS);
    state.tile_reassembly_timeout_ns.store(200000000ull);
    state.tile_reassembly_ewma_ns = 500000000.0;
    state.tile_reassembly_deviation_ns = 300000000.0;

    auto packets = make_uncompressed_packets(state, 1, 3);
    require(packets.size() == 3, "timeout test should use a partial multi-packet tile");
    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
            "first fragment should remain partial");
    require(reassembler.active_entry_count() == 1 && reassembler.next_expiry_deadline_ns(state) != 0,
            "partial tile should expose an expiry deadline");

    std::this_thread::sleep_for(std::chrono::milliseconds(225));
    reassembler.expire_stale_entries(state);
    require(reassembler.active_entry_count() == 0, "stale partial tile should expire");
    require(state.stats.partial_tiles_timed_out.load() == 1 &&
                state.stats.partial_tile_missing_packets.load() == 2,
            "timeout should count the tile and each missing fragment");
    require(!state.retx_queue.empty() && state.stats.partial_tile_retx_queued.load() == 1,
            "timeout should queue immediate repair work");
    require(state.tile_reassembly_timeout_ns.load() == WD_LINK_TILE_REASSEMBLY_MIN_NS,
            "loss should reduce the adaptive timeout to its floor");
    require(state.tile_reassembly_ewma_ns <= static_cast<double>(WD_LINK_TILE_REASSEMBLY_MIN_NS) &&
                state.tile_reassembly_deviation_ns <= static_cast<double>(WD_LINK_TILE_REASSEMBLY_MIN_NS) / 2.0,
            "loss reduction should clamp stale EWMA and deviation state");
    require(reassembler.next_expiry_deadline_ns(state) == 0,
            "empty reassembler should have no expiry deadline");
}

void test_reset_and_completed_buffer_pool_bounds() {
    ClientState state;
    initialize_state(state, 400);
    auto packets = make_uncompressed_packets(state, 2, 4);
    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
            "reset test should create a partial entry");
    require(reassembler.active_entry_count() == 1 && reassembler.slot_count() != 0,
            "partial entry should configure slot tables");
    reassembler.reset();
    require(reassembler.active_entry_count() == 0 && reassembler.active_payload_bytes() == 0 &&
                reassembler.slot_count() == 0,
            "reset should clear active entries, payload accounting, and slot geometry");

    std::vector<uint8_t> oversized;
    oversized.reserve(static_cast<size_t>(WD_WIRE_TILE_MAX_WIDTH) * WD_WIRE_TILE_MAX_HEIGHT *
                          WD_BYTES_PER_PIXEL + 1u);
    reassembler.recycle_completed_tile_buffer(std::move(oversized));
    require(reassembler.recycled_completed_buffer_count() == 0,
            "oversized completed buffers should not be retained");

    for (size_t i = 0; i < 12; ++i)
    {
        std::vector<uint8_t> buffer(1024, static_cast<uint8_t>(i));
        reassembler.recycle_completed_tile_buffer(std::move(buffer));
    }
    require(reassembler.recycled_completed_buffer_count() == 8,
            "completed buffer recycle pool should remain bounded");
}

} // namespace

int main() {
    test_short_header_identity_geometry_and_fragment_counters();
    test_payload_larger_than_budget_drops_without_entry();
    test_partial_timeout_reduces_adaptive_deadline_and_queues_repair();
    test_reset_and_completed_buffer_pool_bounds();
    return 0;
}
