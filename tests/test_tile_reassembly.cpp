#include "tile_reassembly.hpp"

#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

void initialize_state(ClientState& state, uint16_t width = 64, uint16_t height = 64, uint16_t udp_payload_target = 1400) {
    state.config.session_id = 3;
    state.config.connection_token = 0xabcddcba12344321ull;
    state.config.content_epoch = 1;
    state.config.config_epoch = 1;
    state.config.server_udp_port = 5001;
    state.config.width = width;
    state.config.height = height;
    state.config.tile_width = 16;
    state.config.tile_height = 16;
    state.config.tiles_x = static_cast<uint16_t>((width + 15u) / 16u);
    state.config.tiles_y = static_cast<uint16_t>((height + 15u) / 16u);
    state.config.total_tiles = static_cast<uint16_t>(state.config.tiles_x * state.config.tiles_y);
    state.config.udp_payload_target = udp_payload_target;
    state.received_generation.assign(state.config.total_tiles, 0);
    state.retx_queued_generation.assign(state.config.total_tiles, 0);
    state.retx_last_requested_generation.assign(state.config.total_tiles, 0);
    state.retx_last_request_ns.assign(state.config.total_tiles, 0);
    state.retx_inflight_generation.assign(state.config.total_tiles, 0);
    state.retx_inflight_since_ns.assign(state.config.total_tiles, 0);
}

std::vector<uint8_t> make_tile_bytes(uint8_t tile_size, uint32_t seed = 1) {
    uint16_t width = 0;
    uint16_t height = 0;
    require(wd_tile_dimensions_for_size_code(tile_size, &width, &height), "test tile dimensions");
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * WD_BYTES_PER_PIXEL);
    uint32_t value = seed;
    for (uint8_t& byte : bytes)
    {
        value = value * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>(value >> 24u);
    }
    return bytes;
}

std::vector<std::vector<uint8_t>> make_packets(const ClientState& state, uint8_t tile_size, uint16_t tile_id,
                                               uint64_t generation, const std::vector<uint8_t>& tile_bytes,
                                               bool compressed, bool latency) {
    std::vector<uint8_t> wire_payload = tile_bytes;
    if (compressed)
    {
        std::vector<uint8_t> output(wd_zstd_compress_bound(tile_bytes.size()));
        uint32_t output_size = 0;
        require(wd_zstd_compress(tile_bytes.data(), tile_bytes.size(), output.data(), output.size(), 1, &output_size),
                "test compression");
        output.resize(output_size);
        wire_payload = std::move(output);
    }

    const uint16_t target = state.config.udp_payload_target;
    const uint32_t packet_count_u32 =
        (static_cast<uint32_t>(wire_payload.size()) + target - 1u) / target;
    require(packet_count_u32 > 0 && packet_count_u32 <= UINT8_MAX, "test packet count");
    const uint8_t packet_count = static_cast<uint8_t>(packet_count_u32);

    std::vector<std::vector<uint8_t>> packets;
    for (uint8_t packet_id = 0; packet_id < packet_count; ++packet_id)
    {
        const size_t offset = static_cast<size_t>(packet_id) * target;
        const uint16_t payload_size = static_cast<uint16_t>(std::min<size_t>(target, wire_payload.size() - offset));

        wd_udp_tile_packet_decoded header{};
        header.session_id = state.config.session_id;
        header.connection_token = state.config.connection_token;
        header.content_epoch = state.config.content_epoch;
        header.flags = compressed ? WD_UDP_TILE_FLAG_COMPRESSED : 0;
        if (latency && packet_id == 0)
        {
            header.flags |= WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
            header.input_sequence = 77;
        }
        header.tile_size = tile_size;
        header.tile_pkt_id = packet_id;
        header.tile_id = tile_id;
        header.tile_pkt_count = packet_count;
        header.payload_size = payload_size;
        header.tile_payload_size = static_cast<uint16_t>(wire_payload.size());
        header.tile_generation = generation;
        const uint16_t header_size = wd_udp_tile_header_size_for_flags(header.flags);
        std::vector<uint8_t> packet(header_size + payload_size);
        require(wd_udp_tile_packet_encode_header(packet.data(), packet.size(), &header), "test tile header encode");
        std::memcpy(packet.data() + header_size, wire_payload.data() + offset, payload_size);
        packets.push_back(std::move(packet));
    }
    return packets;
}

std::vector<uint8_t> make_first_32x32_packet(uint64_t generation) {
    ClientState state;
    initialize_state(state);
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_32x32);
    return make_packets(state, WD_TILE_32x32, 0, generation, bytes, false, false).front();
}

void test_large_tile_marks_all_covered_repairs_inflight() {
    ClientState state;
    initialize_state(state);
    const uint64_t request_ns = wd_now_ns() - 1000000ull;
    for (uint16_t base_id : std::array<uint16_t, 4>{0, 1, 4, 5})
    {
        state.retx_last_requested_generation[base_id] = 5;
        state.retx_last_request_ns[base_id] = request_ns;
    }

    TileReassembler reassembler;
    std::vector<uint8_t> packet = make_first_32x32_packet(6);
    CompletedTile completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
    require(!completed.valid, "one fragment should not complete the tile");

    for (uint16_t base_id : std::array<uint16_t, 4>{0, 1, 4, 5})
    {
        require(state.retx_inflight_generation[base_id] == 6,
                "every base tile covered by a large tile should be marked in flight");
        require(state.retx_inflight_since_ns[base_id] != 0, "covered tile should record inflight start");
    }
}

void test_newer_generation_replaces_covered_inflight_state() {
    ClientState state;
    initialize_state(state);
    TileReassembler reassembler;

    std::vector<uint8_t> generation_six = make_first_32x32_packet(6);
    reassembler.process_udp_packet(state, generation_six.data(), generation_six.size());

    std::vector<uint8_t> generation_seven = make_first_32x32_packet(7);
    reassembler.process_udp_packet(state, generation_seven.data(), generation_seven.size());

    for (uint16_t base_id : std::array<uint16_t, 4>{0, 1, 4, 5})
    {
        require(state.retx_inflight_generation[base_id] == 7,
                "newer large-tile generation should replace covered inflight state");
    }
}

void test_out_of_order_and_duplicate_fragments() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_16x16, 11);
    const auto packets = make_packets(state, WD_TILE_16x16, 3, 8, bytes, false, true);
    require(packets.size() == 3, "uncompressed tile should be fragmented for test");

    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, packets[1].data(), packets[1].size()).valid,
            "out-of-order middle packet should remain partial");
    require(!reassembler.process_udp_packet(state, packets[1].data(), packets[1].size()).valid,
            "duplicate packet should not advance completion");
    require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
            "two unique packets should remain partial");
    CompletedTile completed = reassembler.process_udp_packet(state, packets[2].data(), packets[2].size());
    require(completed.valid, "final unique fragment should complete tile");
    require(completed.tile_bytes == bytes, "out-of-order tile payload should round trip");
    require(completed.input_sequence == 77, "latency input sequence should survive reassembly");
}

void test_compressed_round_trip_for_each_tile_size() {
    for (uint8_t tile_size : std::array<uint8_t, 4>{WD_TILE_128x64, WD_TILE_64x64, WD_TILE_32x32, WD_TILE_16x16})
    {
        ClientState state;
        initialize_state(state, 256, 128, 256);
        std::vector<uint8_t> bytes = make_tile_bytes(tile_size, 100u + tile_size);
        const auto packets = make_packets(state, tile_size, 0, 12 + tile_size, bytes, true, true);
        require(!packets.empty(), "compressed packets should be produced");

        TileReassembler reassembler;
        CompletedTile completed;
        for (const auto& packet : packets)
        {
            completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
        }
        require(completed.valid, "compressed tile should complete");
        require(completed.tile_bytes == bytes, "compressed tile should round trip");
    }
}

void test_rejects_malformed_fragment_without_poisoning_entry() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_16x16, 44);
    auto packets = make_packets(state, WD_TILE_16x16, 1, 9, bytes, false, false);
    require(packets.size() == 3, "malformed fragment test packet count");

    wd_udp_tile_packet_header malformed{};
    std::memcpy(&malformed, packets[1].data(), sizeof(malformed));
    malformed.payload_size--;
    packets[1].resize(sizeof(malformed) + malformed.payload_size);
    std::memcpy(packets[1].data(), &malformed, sizeof(malformed));

    TileReassembler reassembler;
    const uint64_t invalid_before = state.stats.udp_ignored_invalid.load();
    require(!reassembler.process_udp_packet(state, packets[1].data(), packets[1].size()).valid,
            "malformed middle fragment should be rejected");
    require(state.stats.udp_ignored_invalid.load() == invalid_before + 1, "malformed packet should increment invalid count");

    const auto valid_packets = make_packets(state, WD_TILE_16x16, 1, 9, bytes, false, false);
    CompletedTile completed;
    for (const auto& packet : valid_packets)
    {
        completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
    }
    require(completed.valid && completed.tile_bytes == bytes, "valid fragments after malformed input should complete");
}

void test_reconfiguration_discards_partial_slot_layout() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const auto old_packets = make_packets(state, WD_TILE_16x16, 0, 2, make_tile_bytes(WD_TILE_16x16, 5), false, false);
    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, old_packets[0].data(), old_packets[0].size()).valid,
            "old configuration should retain partial entry");

    initialize_state(state, 128, 64, 400);
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_16x16, 6);
    const auto new_packets = make_packets(state, WD_TILE_16x16, 7, 3, bytes, false, false);
    CompletedTile completed;
    for (const auto& packet : new_packets)
    {
        completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
    }
    require(completed.valid && completed.tile_id == 7 && completed.tile_bytes == bytes,
            "reconfigured slot layout should accept new geometry");
}

void test_drops_fully_redundant_generation() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    state.received_generation[2] = 9;
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_16x16, 91);
    const auto packets = make_packets(state, WD_TILE_16x16, 2, 9, bytes, false, false);

    TileReassembler reassembler;
    const uint64_t ignored_before = state.stats.udp_ignored_old_generation.load();
    for (const auto& packet : packets)
    {
        require(!reassembler.process_udp_packet(state, packet.data(), packet.size()).valid,
                "equal generation should be discarded before reassembly");
    }
    require(state.stats.udp_ignored_old_generation.load() == ignored_before + packets.size(),
            "redundant packets should be counted as ignored generations");
}

void test_large_tile_accepts_mixed_equal_and_older_coverage() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    state.received_generation[0] = 6;
    state.received_generation[1] = 6;
    state.received_generation[4] = 6;
    state.received_generation[5] = 5;
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_32x32, 92);
    const auto packets = make_packets(state, WD_TILE_32x32, 0, 6, bytes, false, false);

    TileReassembler reassembler;
    CompletedTile completed;
    for (const auto& packet : packets)
    {
        completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
    }
    require(completed.valid && completed.tile_bytes == bytes,
            "large tile should repair base tiles that are still older");
}

void test_large_tile_rejects_when_any_covered_tile_is_newer() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    state.received_generation[0] = 7;
    const std::vector<uint8_t> bytes = make_tile_bytes(WD_TILE_32x32, 93);
    const auto packets = make_packets(state, WD_TILE_32x32, 0, 6, bytes, false, false);

    TileReassembler reassembler;
    for (const auto& packet : packets)
    {
        require(!reassembler.process_udp_packet(state, packet.data(), packet.size()).valid,
                "large tile must not overwrite a newer covered base tile");
    }
}

void test_tracks_only_active_partial_entries() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const std::vector<uint8_t> bytes_a = make_tile_bytes(WD_TILE_16x16, 101);
    const std::vector<uint8_t> bytes_b = make_tile_bytes(WD_TILE_16x16, 102);
    const auto packets_a = make_packets(state, WD_TILE_16x16, 0, 20, bytes_a, false, false);
    const auto packets_b = make_packets(state, WD_TILE_16x16, 1, 21, bytes_b, false, false);

    TileReassembler reassembler;
    require(reassembler.active_entry_count() == 0, "new reassembler should not contain active entries");
    require(!reassembler.process_udp_packet(state, packets_a[0].data(), packets_a[0].size()).valid,
            "first partial tile should remain incomplete");
    require(reassembler.slot_count() == 22, "64x64 configuration should allocate compact lookup slots");
    require(reassembler.active_entry_count() == 1, "one partial tile should create one active entry");

    require(!reassembler.process_udp_packet(state, packets_b[0].data(), packets_b[0].size()).valid,
            "second partial tile should remain incomplete");
    require(reassembler.active_entry_count() == 2, "two partial tiles should create two active entries");

    CompletedTile completed;
    for (size_t i = 1; i < packets_a.size(); ++i)
    {
        completed = reassembler.process_udp_packet(state, packets_a[i].data(), packets_a[i].size());
    }
    require(completed.valid && completed.tile_bytes == bytes_a, "completed active entry should round trip");
    require(reassembler.active_entry_count() == 1, "completion should remove only the completed active entry");

    reassembler.reset();
    require(reassembler.active_entry_count() == 0, "reset should discard active entries");
    require(reassembler.slot_count() == 0, "reset should discard lookup slots");
}

void test_reuses_bounded_reassembly_storage() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const std::vector<uint8_t> bytes_a = make_tile_bytes(WD_TILE_16x16, 111);
    const std::vector<uint8_t> bytes_b = make_tile_bytes(WD_TILE_16x16, 112);
    const auto packets_a = make_packets(state, WD_TILE_16x16, 0, 30, bytes_a, false, false);
    const auto packets_b = make_packets(state, WD_TILE_16x16, 1, 31, bytes_b, false, false);

    TileReassembler reassembler;
    CompletedTile first;
    for (const auto& packet : packets_a)
    {
        first = reassembler.process_udp_packet(state, packet.data(), packet.size());
    }
    require(first.valid && first.tile_bytes == bytes_a, "first tile should complete before buffer reuse");
    require(reassembler.recycled_entry_count() == 1, "completed fragment storage should enter the recycle pool");

    const size_t first_capacity = first.tile_bytes.capacity();
    reassembler.recycle_completed_tile_buffer(std::move(first.tile_bytes));
    require(reassembler.recycled_completed_buffer_count() == 1, "completed output buffer should enter the recycle pool");

    require(!reassembler.process_udp_packet(state, packets_b[0].data(), packets_b[0].size()).valid,
            "second tile should begin as partial");
    require(reassembler.recycled_entry_count() == 0, "new partial tile should reuse recycled fragment storage");

    CompletedTile second;
    for (size_t i = 1; i < packets_b.size(); ++i)
    {
        second = reassembler.process_udp_packet(state, packets_b[i].data(), packets_b[i].size());
    }
    require(second.valid && second.tile_bytes == bytes_b, "second tile should complete using recycled storage");
    require(second.tile_bytes.capacity() >= first_capacity, "completed output capacity should be retained for reuse");
    require(reassembler.recycled_completed_buffer_count() == 0, "reused output buffer should leave the recycle pool");
}


void test_bounds_active_reassembly_memory_and_evicts_oldest() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    TileReassembler reassembler(2, 2048);

    for (uint16_t tile_id = 0; tile_id < 3; ++tile_id)
    {
        const auto packets = make_packets(state, WD_TILE_16x16, tile_id, 40 + tile_id,
                                          make_tile_bytes(WD_TILE_16x16, 120 + tile_id), false, false);
        require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
                "budget test should retain partial tiles");
    }

    require(reassembler.active_entry_count() == 2, "active-entry budget should be enforced");
    require(reassembler.active_payload_bytes() <= 2048, "active payload byte budget should be enforced");
    require(state.stats.reassembly_budget_evictions.load() == 1, "oldest partial tile should be evicted");
    require(!state.retx_queue.empty(), "budget eviction should immediately queue repair");
}

void test_decompression_failure_immediately_queues_repair() {
    ClientState state;
    initialize_state(state, 64, 64, 400);

    wd_udp_tile_packet_decoded header{};
    header.session_id = state.config.session_id;
    header.connection_token = state.config.connection_token;
    header.content_epoch = state.config.content_epoch;
    header.flags = WD_UDP_TILE_FLAG_COMPRESSED;
    header.tile_size = WD_TILE_16x16;
    header.tile_id = 0;
    header.tile_pkt_count = 1;
    header.payload_size = 32;
    header.tile_payload_size = 32;
    header.tile_generation = 50;
    std::vector<uint8_t> packet(WD_UDP_TILE_HEADER_MIN_SIZE + header.payload_size, 0x5a);
    require(wd_udp_tile_packet_encode_header(packet.data(), packet.size(), &header),
            "invalid compressed test header");

    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, packet.data(), packet.size()).valid,
            "corrupt compressed payload should not complete");
    require(state.stats.tile_decompress_failures.load() == 1, "decompression failure should be counted");
    require(!state.retx_queue.empty(), "decompression failure should queue immediate repair");
    require(reassembler.active_entry_count() == 0, "corrupt completed entry should be removed");
}

void test_conflicting_duplicate_fragment_poisoning_is_rejected() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const auto packets = make_packets(state, WD_TILE_16x16, 0, 60,
                                      make_tile_bytes(WD_TILE_16x16, 130), false, false);
    require(packets.size() > 1, "conflict test requires a fragmented tile");

    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
            "first fragment should remain partial");
    std::vector<uint8_t> conflicting = packets[0];
    conflicting.back() ^= 0xff;
    require(!reassembler.process_udp_packet(state, conflicting.data(), conflicting.size()).valid,
            "conflicting duplicate should be rejected");
    require(state.stats.tile_fragment_conflicts.load() == 1, "fragment conflict should be counted");
    require(reassembler.active_entry_count() == 0, "poisoned partial tile should be discarded");
    require(!state.retx_queue.empty(), "fragment conflict should queue repair");
}


void test_conflicting_duplicate_fragment_zero_metadata_is_rejected() {
    ClientState state;
    initialize_state(state, 64, 64, 400);
    const auto packets = make_packets(state, WD_TILE_16x16, 0, 61,
                                      make_tile_bytes(WD_TILE_16x16, 131), false, true);
    require(packets.size() > 1, "metadata conflict test requires a fragmented tile");

    TileReassembler reassembler;
    require(!reassembler.process_udp_packet(state, packets[0].data(), packets[0].size()).valid,
            "first fragment should remain partial");

    std::vector<uint8_t> conflicting = packets[0];
    wd_udp_tile_input_sequence_extension extension{};
    std::memcpy(&extension, conflicting.data() + sizeof(wd_udp_tile_packet_header), sizeof(extension));
    extension.input_sequence++;
    std::memcpy(conflicting.data() + sizeof(wd_udp_tile_packet_header), &extension, sizeof(extension));

    require(!reassembler.process_udp_packet(state, conflicting.data(), conflicting.size()).valid,
            "duplicate fragment zero with different metadata should be rejected");
    require(state.stats.tile_fragment_conflicts.load() == 1,
            "fragment-zero metadata conflict should be counted");
    require(reassembler.active_entry_count() == 0,
            "metadata-conflicted partial tile should be discarded");
    require(!state.retx_queue.empty(), "metadata conflict should queue immediate repair");
}

} // namespace

int main() {
    test_large_tile_marks_all_covered_repairs_inflight();
    test_newer_generation_replaces_covered_inflight_state();
    test_out_of_order_and_duplicate_fragments();
    test_compressed_round_trip_for_each_tile_size();
    test_rejects_malformed_fragment_without_poisoning_entry();
    test_reconfiguration_discards_partial_slot_layout();
    test_drops_fully_redundant_generation();
    test_large_tile_accepts_mixed_equal_and_older_coverage();
    test_large_tile_rejects_when_any_covered_tile_is_newer();
    test_tracks_only_active_partial_entries();
    test_reuses_bounded_reassembly_storage();
    test_bounds_active_reassembly_memory_and_evicts_oldest();
    test_decompression_failure_immediately_queues_repair();
    test_conflicting_duplicate_fragment_poisoning_is_rejected();
    test_conflicting_duplicate_fragment_zero_metadata_is_rejected();
    return 0;
}
