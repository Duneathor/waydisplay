#include "tile_reassembly.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace waydisplay {
namespace {

constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_MIN_NS     = 100ull * 1000ull * 1000ull;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS = 250ull * 1000ull * 1000ull;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_MAX_NS     = 450ull * 1000ull * 1000ull;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_SLACK_NS   = 50ull * 1000ull * 1000ull;
constexpr size_t   MAX_REASSEMBLY_RETRANSMIT_QUEUE_DEPTH = 256;

uint64_t clamp_tile_reassembly_timeout_ns(uint64_t ns) {
    return std::max(TILE_REASSEMBLY_TIMEOUT_MIN_NS, std::min(TILE_REASSEMBLY_TIMEOUT_MAX_NS, ns));
}

uint64_t current_tile_reassembly_timeout_ns(const ClientState& state) {
    const uint64_t timeout_ns = state.tile_reassembly_timeout_ns.load(std::memory_order_relaxed);
    if (timeout_ns == 0)
    {
        return TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS;
    }

    return clamp_tile_reassembly_timeout_ns(timeout_ns);
}

void update_tile_reassembly_timeout(ClientState& state, uint64_t sample_ns, bool response_sample) {
    if (sample_ns == 0 || response_sample)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(state.tile_reassembly_timeout_mutex);

    if (state.tile_reassembly_ewma_ns <= 0.0)
    {
        state.tile_reassembly_ewma_ns      = static_cast<double>(sample_ns);
        state.tile_reassembly_deviation_ns = static_cast<double>(sample_ns) / 2.0;
    }
    else
    {
        const double sample = static_cast<double>(sample_ns);
        const double delta  = sample - state.tile_reassembly_ewma_ns;
        state.tile_reassembly_ewma_ns += 0.125 * delta;
        state.tile_reassembly_deviation_ns += 0.25 * (std::abs(delta) - state.tile_reassembly_deviation_ns);
    }

    const double jittered_tile_ns = state.tile_reassembly_ewma_ns + 2.0 * state.tile_reassembly_deviation_ns +
                                    static_cast<double>(TILE_REASSEMBLY_TIMEOUT_SLACK_NS);
    const uint64_t target_ns = clamp_tile_reassembly_timeout_ns(static_cast<uint64_t>(jittered_tile_ns));

    const uint64_t old_ns = current_tile_reassembly_timeout_ns(state);
    if (old_ns != target_ns)
    {
        state.tile_reassembly_timeout_ns.store(target_ns, std::memory_order_relaxed);
        state.stats.tile_reassembly_timeout_updates.fetch_add(1, std::memory_order_relaxed);
    }
}

void reduce_tile_reassembly_timeout_after_loss(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.tile_reassembly_timeout_mutex);

    const uint64_t old_ns = current_tile_reassembly_timeout_ns(state);
    uint64_t target_ns = old_ns * 3ull / 4ull;
    if (old_ns > TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS && target_ns > TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS)
    {
        target_ns = TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS;
    }
    target_ns = clamp_tile_reassembly_timeout_ns(target_ns);

    if (state.tile_reassembly_ewma_ns > static_cast<double>(target_ns))
    {
        state.tile_reassembly_ewma_ns = static_cast<double>(target_ns);
    }
    if (state.tile_reassembly_deviation_ns > static_cast<double>(target_ns) / 2.0)
    {
        state.tile_reassembly_deviation_ns = static_cast<double>(target_ns) / 2.0;
    }

    if (old_ns != target_ns)
    {
        state.tile_reassembly_timeout_ns.store(target_ns, std::memory_order_relaxed);
        state.stats.tile_reassembly_timeout_updates.fetch_add(1, std::memory_order_relaxed);
    }
}

void clear_retx_inflight(ClientState& state, uint16_t tile_id, uint64_t generation) {
    std::lock_guard<std::mutex> lock(state.retx_mutex);

    if (state.retx_inflight_generation.size() == state.config.total_tiles &&
        state.retx_inflight_since_ns.size() == state.config.total_tiles && tile_id < state.retx_inflight_generation.size() &&
        state.retx_inflight_generation[tile_id] == generation)
    {
        state.retx_inflight_generation[tile_id] = 0;
        state.retx_inflight_since_ns[tile_id]  = 0;
    }
}

bool enqueue_retx_for_partial_timeout(ClientState& state, uint16_t tile_id, uint64_t generation) {
    if (state.stream_config.mode == ClientStreamMode::Live)
    {
        return false;
    }

    std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
    std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

    const uint16_t total_tiles = state.config.total_tiles;
    if (total_tiles == 0 || tile_id >= total_tiles || state.displayed_generation.size() != total_tiles)
    {
        return false;
    }

    if (state.displayed_generation[tile_id] >= generation)
    {
        return false;
    }

    if (state.retx_queued_generation.size() != total_tiles)
    {
        state.retx_queued_generation.assign(total_tiles, 0);
    }

    if (state.retx_last_requested_generation.size() != total_tiles)
    {
        state.retx_last_requested_generation.assign(total_tiles, 0);
    }

    if (state.retx_last_request_ns.size() != total_tiles)
    {
        state.retx_last_request_ns.assign(total_tiles, 0);
    }

    if (state.retx_inflight_generation.size() != total_tiles)
    {
        state.retx_inflight_generation.assign(total_tiles, 0);
    }

    if (state.retx_inflight_since_ns.size() != total_tiles)
    {
        state.retx_inflight_since_ns.assign(total_tiles, 0);
    }

    if (state.retx_queued_generation[tile_id] == 0)
    {
        while (state.retx_queue.size() >= MAX_REASSEMBLY_RETRANSMIT_QUEUE_DEPTH)
        {
            const uint16_t dropped_tile_id = state.retx_queue.front();
            state.retx_queue.pop_front();

            if (dropped_tile_id < state.retx_queued_generation.size())
            {
                state.retx_queued_generation[dropped_tile_id] = 0;
            }
        }

        state.retx_queue.push_back(tile_id);
    }

    if (state.retx_queued_generation[tile_id] < generation)
    {
        state.retx_queued_generation[tile_id] = generation;
    }

    if (state.retx_inflight_generation[tile_id] == generation)
    {
        state.retx_inflight_generation[tile_id] = 0;
        state.retx_inflight_since_ns[tile_id]  = 0;
    }

    return true;
}

bool packet_header_valid(const wd_udp_tile_packet_header& header, size_t packet_size, uint16_t udp_payload_target,
                         const wd_server_config_payload& config) {
    if (header.tile_id >= config.total_tiles)
    {
        return false;
    }

    if (header.tile_pkt_count == 0)
    {
        return false;
    }

    if (header.tile_pkt_id >= header.tile_pkt_count)
    {
        return false;
    }

    if (header.payload_size == 0)
    {
        return false;
    }

    if (sizeof(wd_udp_tile_packet_header) + header.payload_size > packet_size)
    {
        return false;
    }

    if (header.compressed_tile_size == 0)
    {
        return false;
    }

    const size_t max_compressed_size =
        wd_zstd_compress_bound(static_cast<size_t>(config.tile_width) * static_cast<size_t>(config.tile_height) * WD_BYTES_PER_PIXEL);

    if (header.compressed_tile_size > max_compressed_size)
    {
        return false;
    }

    const uint32_t offset = static_cast<uint32_t>(header.tile_pkt_id) * udp_payload_target;

    if (offset + header.payload_size > header.compressed_tile_size)
    {
        return false;
    }

    return true;
}

} // namespace

TileReassembler::TileReassembler() = default;

void TileReassembler::reset() {
    entries_.clear();
}

uint64_t TileReassembler::count_missing_packets(const Entry& entry) {
    if (!entry.active || entry.received.empty())
    {
        return 0;
    }

    return static_cast<uint64_t>(std::count(entry.received.begin(), entry.received.end(), 0));
}

void TileReassembler::expire_entry(ClientState& state, Entry& entry) {
    if (!entry.active)
    {
        return;
    }

    const uint64_t missing_packets = count_missing_packets(entry);

    state.stats.partial_tiles_timed_out.fetch_add(1, std::memory_order_relaxed);
    state.stats.partial_tile_missing_packets.fetch_add(missing_packets, std::memory_order_relaxed);
    reduce_tile_reassembly_timeout_after_loss(state);

    if (enqueue_retx_for_partial_timeout(state, entry.tile_id, entry.generation))
    {
        state.stats.partial_tile_retx_queued.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        clear_retx_inflight(state, entry.tile_id, entry.generation);
    }

    entry = Entry{};
}

void TileReassembler::expire_stale_entries(ClientState& state) {
    if (entries_.empty())
    {
        return;
    }

    const uint64_t now_ns = wd_now_ns();

    for (Entry& entry : entries_)
    {
        if (entry.active && entry.first_packet_ns != 0 && now_ns - entry.first_packet_ns > current_tile_reassembly_timeout_ns(state))
        {
            expire_entry(state, entry);
        }
    }
}

CompletedTile TileReassembler::process_udp_packet(ClientState& state, const uint8_t* packet, size_t packet_size) {
    CompletedTile completed{};

    if (!packet || packet_size < sizeof(wd_udp_tile_packet_header))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    wd_udp_tile_packet_header header{};
    std::memcpy(&header, packet, sizeof(header));

    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (!packet_header_valid(header, packet_size, udp_payload_target, state.config))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    {
        std::lock_guard<std::mutex> lock(state.generation_mutex);

        if (header.tile_generation < state.displayed_generation[header.tile_id])
        {
            state.stats.udp_ignored_old_generation.fetch_add(1, std::memory_order_relaxed);
            return completed;
        }
    }

    if (entries_.size() != state.config.total_tiles)
    {
        entries_.assign(state.config.total_tiles, Entry{});
    }

    Entry& entry = entries_[header.tile_id];

    const uint64_t now_ns = wd_now_ns();

    if (entry.active && header.tile_generation < entry.generation)
    {
        state.stats.udp_ignored_old_generation.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    if (entry.active && entry.generation == header.tile_generation && entry.first_packet_ns != 0 &&
        now_ns - entry.first_packet_ns > current_tile_reassembly_timeout_ns(state))
    {
        expire_entry(state, entry);
    }

    if (!entry.active || entry.generation != header.tile_generation)
    {
        entry                   = Entry{};
        entry.active            = true;
        entry.tile_id           = header.tile_id;
        entry.generation        = header.tile_generation;
        entry.tile_timestamp_ns = header.tile_timestamp_ns;
        entry.packet_count      = header.tile_pkt_count;
        entry.compressed_size   = header.compressed_tile_size;
        entry.first_packet_ns   = now_ns;

        uint64_t retx_response_ns = 0;
        {
            std::lock_guard<std::mutex> lock(state.retx_mutex);

            if (state.retx_last_requested_generation.size() == state.config.total_tiles &&
                state.retx_last_request_ns.size() == state.config.total_tiles &&
                state.retx_last_requested_generation[entry.tile_id] != 0 &&
                state.retx_last_requested_generation[entry.tile_id] <= entry.generation &&
                state.retx_last_request_ns[entry.tile_id] != 0 &&
                now_ns >= state.retx_last_request_ns[entry.tile_id])
            {
                retx_response_ns = now_ns - state.retx_last_request_ns[entry.tile_id];
                state.retx_inflight_grace_ns = clamp_tile_reassembly_timeout_ns(retx_response_ns + retx_response_ns / 2);
            }

            if (state.retx_inflight_generation.size() == state.config.total_tiles &&
                state.retx_inflight_since_ns.size() == state.config.total_tiles)
            {
                state.retx_inflight_generation[entry.tile_id] = entry.generation;
                state.retx_inflight_since_ns[entry.tile_id]  = entry.first_packet_ns;
            }
        }

        if (retx_response_ns != 0)
        {
            state.stats.retx_response_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.retx_response_sum_ns.fetch_add(retx_response_ns, std::memory_order_relaxed);
            update_tile_reassembly_timeout(state, retx_response_ns, true);
        }

        entry.compressed.assign(header.compressed_tile_size, 0);
        entry.received.assign(header.tile_pkt_count, 0);
        entry.received_count = 0;
    }

    if (entry.packet_count != header.tile_pkt_count || entry.compressed_size != header.compressed_tile_size)
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    const uint32_t offset = static_cast<uint32_t>(header.tile_pkt_id) * udp_payload_target;

    const uint8_t* payload = packet + sizeof(wd_udp_tile_packet_header);

    if (!entry.received[header.tile_pkt_id])
    {
        std::memcpy(entry.compressed.data() + offset, payload, header.payload_size);

        entry.received[header.tile_pkt_id] = 1;
        entry.received_count++;
    }

    if (entry.received_count != entry.packet_count)
    {
        return completed;
    }

    const size_t uncompressed_tile_bytes =
        static_cast<size_t>(state.config.tile_width) * static_cast<size_t>(state.config.tile_height) * WD_BYTES_PER_PIXEL;

    completed.tile_bytes.assign(uncompressed_tile_bytes, 0);

    const bool ok = wd_zstd_decompress(entry.compressed.data(), entry.compressed.size(), completed.tile_bytes.data(),
                                       completed.tile_bytes.size(), uncompressed_tile_bytes);

    if (!ok)
    {
        std::fprintf(stderr, "failed to decompress tile %u generation %llu\n", entry.tile_id,
                     static_cast<unsigned long long>(entry.generation));

        clear_retx_inflight(state, entry.tile_id, entry.generation);

        entry = Entry{};
        return completed;
    }

    completed.valid                  = true;
    completed.tile_id                = entry.tile_id;
    completed.generation             = entry.generation;
    completed.tile_timestamp_ns      = entry.tile_timestamp_ns;
    completed.input_sequence         = entry.input_sequence;
    completed.first_packet_ns        = entry.first_packet_ns;
    completed.completed_timestamp_ns = wd_now_ns();
    completed.compressed_size        = entry.compressed_size;
    completed.packet_count           = entry.packet_count;

    if (completed.completed_timestamp_ns >= completed.first_packet_ns)
    {
        update_tile_reassembly_timeout(state, completed.completed_timestamp_ns - completed.first_packet_ns, false);
    }

    clear_retx_inflight(state, entry.tile_id, entry.generation);

    entry = Entry{};

    return completed;
}

} // namespace waydisplay
