#include "tile_reassembly.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"

namespace waydisplay {
namespace {

bool packet_header_valid(const wd_udp_tile_packet_header& header,
                         size_t packet_size,
                         uint16_t udp_payload_target,
                         const wd_server_config_payload& config) {
    if (header.tile_id >= config.total_tiles) {
        return false;
    }

    if (header.tile_pkt_count == 0) {
        return false;
    }

    if (header.tile_pkt_id >= header.tile_pkt_count) {
        return false;
    }

    if (header.payload_size == 0) {
        return false;
    }

    if (sizeof(wd_udp_tile_packet_header) + header.payload_size > packet_size) {
        return false;
    }

    if (header.compressed_tile_size == 0) {
        return false;
    }

    const size_t max_compressed_size =
        wd_zstd_compress_bound(static_cast<size_t>(config.tile_width) *
                               static_cast<size_t>(config.tile_height) *
                               WD_BYTES_PER_PIXEL);

    if (header.compressed_tile_size > max_compressed_size) {
        return false;
    }

    const uint32_t offset =
    static_cast<uint32_t>(header.tile_pkt_id) * udp_payload_target;

    if (offset + header.payload_size > header.compressed_tile_size) {
        return false;
    }

    return true;
}

} // namespace

TileReassembler::TileReassembler() = default;

void TileReassembler::reset() {
    entries_.clear();
}

CompletedTile TileReassembler::process_udp_packet(ClientState& state,
                                                  const uint8_t* packet,
                                                  size_t packet_size) {
    CompletedTile completed{};

    if (!packet || packet_size < sizeof(wd_udp_tile_packet_header)) {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    wd_udp_tile_packet_header header{};
    std::memcpy(&header, packet, sizeof(header));

    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0) {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (!packet_header_valid(header, packet_size, udp_payload_target, state.config)) {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    {
        std::lock_guard<std::mutex> lock(state.generation_mutex);

        if (header.tile_generation <
            state.displayed_generation[header.tile_id]) {
            state.stats.udp_ignored_old_generation.fetch_add(
                1,
                std::memory_order_relaxed);
            return completed;
        }
    }

    if (entries_.size() != state.config.total_tiles) {
        entries_.assign(state.config.total_tiles, Entry{});
    }

    Entry& entry = entries_[header.tile_id];

    if (!entry.active || entry.generation != header.tile_generation) {
        entry = Entry{};
        entry.active = true;
        entry.tile_id = header.tile_id;
        entry.generation = header.tile_generation;
        entry.packet_count = header.tile_pkt_count;
        entry.compressed_size = header.compressed_tile_size;
        entry.first_packet_ns = wd_now_ns();
        entry.compressed.assign(header.compressed_tile_size, 0);
        entry.received.assign(header.tile_pkt_count, 0);
        entry.received_count = 0;
    }

    if (entry.packet_count != header.tile_pkt_count ||
        entry.compressed_size != header.compressed_tile_size) {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    const uint32_t offset =
        static_cast<uint32_t>(header.tile_pkt_id) * udp_payload_target;

    const uint8_t* payload = packet + sizeof(wd_udp_tile_packet_header);

    if (!entry.received[header.tile_pkt_id]) {
        std::memcpy(entry.compressed.data() + offset,
                    payload,
                    header.payload_size);

        entry.received[header.tile_pkt_id] = 1;
        entry.received_count++;
    }

    if (entry.received_count != entry.packet_count) {
        return completed;
    }

    const size_t uncompressed_tile_bytes =
        static_cast<size_t>(state.config.tile_width) *
        static_cast<size_t>(state.config.tile_height) *
        WD_BYTES_PER_PIXEL;

    completed.tile_bytes.assign(uncompressed_tile_bytes, 0);

    const bool ok = wd_zstd_decompress(entry.compressed.data(),
                                       entry.compressed.size(),
                                       completed.tile_bytes.data(),
                                       completed.tile_bytes.size(),
                                       uncompressed_tile_bytes);

    if (!ok) {
        std::fprintf(stderr,
                     "failed to decompress tile %u generation %llu\n",
                     entry.tile_id,
                     static_cast<unsigned long long>(entry.generation));
        entry = Entry{};
        return completed;
    }

    completed.valid = true;
    completed.tile_id = entry.tile_id;
    completed.generation = entry.generation;

    entry = Entry{};

    return completed;
}

} // namespace waydisplay
