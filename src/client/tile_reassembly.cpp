#include "tile_reassembly.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
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
#include <functional>

namespace waydisplay {
namespace {

constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_MIN_NS     = WD_LINK_TILE_REASSEMBLY_MIN_NS;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS = WD_LINK_TILE_REASSEMBLY_DEFAULT_NS;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_MAX_NS     = WD_LINK_TILE_REASSEMBLY_MAX_NS;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_SLACK_NS   = 50ull * 1000ull * 1000ull;

uint64_t tile_reassembly_floor_ns(const ClientState& state) {
    uint64_t floor_ns = state.tile_reassembly_floor_ns.load(std::memory_order_relaxed);
    if (floor_ns == 0)
    {
        floor_ns = TILE_REASSEMBLY_TIMEOUT_MIN_NS;
    }
    return std::max(TILE_REASSEMBLY_TIMEOUT_MIN_NS, std::min(TILE_REASSEMBLY_TIMEOUT_MAX_NS, floor_ns));
}

uint64_t clamp_tile_reassembly_timeout_ns(const ClientState& state, uint64_t ns) {
    return std::max(tile_reassembly_floor_ns(state), std::min(TILE_REASSEMBLY_TIMEOUT_MAX_NS, ns));
}

uint64_t clamp_retransmit_inflight_grace_ns(uint64_t ns) {
    return std::max<uint64_t>(WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS,
                              std::min<uint64_t>(WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS, ns));
}

uint64_t current_tile_reassembly_timeout_ns(const ClientState& state) {
    const uint64_t timeout_ns = state.tile_reassembly_timeout_ns.load(std::memory_order_relaxed);
    if (timeout_ns == 0)
    {
        return std::max(tile_reassembly_floor_ns(state), TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS);
    }

    return clamp_tile_reassembly_timeout_ns(state, timeout_ns);
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
    const uint64_t target_ns = clamp_tile_reassembly_timeout_ns(state, static_cast<uint64_t>(jittered_tile_ns));

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
    target_ns = clamp_tile_reassembly_timeout_ns(state, target_ns);

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


bool tile_dimensions_from_header(const wd_udp_tile_packet_decoded& header, uint16_t& tile_width, uint16_t& tile_height) {
    return wd_tile_dimensions_for_size_code(header.tile_size, &tile_width, &tile_height);
}

uint16_t tile_base_id_for_header(const wd_udp_tile_packet_decoded& header, const wd_server_config_payload& config) {
    uint16_t tile_width = 0;
    uint16_t tile_height = 0;
    if (!tile_dimensions_from_header(header, tile_width, tile_height) || config.tile_width == 0 || config.tile_height == 0)
    {
        return UINT16_MAX;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(config.width, tile_width);
    if (tiles_x == 0)
    {
        return UINT16_MAX;
    }

    const uint32_t x = wd_tile_start_x_for_tile(header.tile_id, tiles_x, tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(header.tile_id, tiles_x, tile_height);
    const uint32_t bx = x / config.tile_width;
    const uint32_t by = y / config.tile_height;
    const uint32_t base_id = by * static_cast<uint32_t>(config.tiles_x) + bx;
    return base_id < config.total_tiles ? static_cast<uint16_t>(base_id) : UINT16_MAX;
}

void for_each_base_tile_covered(const wd_udp_tile_packet_decoded& header, const wd_server_config_payload& config,
                                const std::function<void(uint16_t)>& fn) {
    uint16_t tile_width = 0;
    uint16_t tile_height = 0;
    if (!tile_dimensions_from_header(header, tile_width, tile_height) || config.tile_width == 0 || config.tile_height == 0)
    {
        return;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(config.width, tile_width);
    if (tiles_x == 0)
    {
        return;
    }

    const uint32_t x = wd_tile_start_x_for_tile(header.tile_id, tiles_x, tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(header.tile_id, tiles_x, tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(config.width, header.tile_id, tiles_x, tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(config.height, header.tile_id, tiles_x, tile_height);
    if (w == 0 || h == 0)
    {
        return;
    }

    uint32_t bx0 = x / config.tile_width;
    uint32_t by0 = y / config.tile_height;
    uint32_t bx1 = (x + w - 1u) / config.tile_width;
    uint32_t by1 = (y + h - 1u) / config.tile_height;
    if (bx1 >= config.tiles_x)
    {
        bx1 = static_cast<uint32_t>(config.tiles_x) - 1u;
    }
    if (by1 >= config.tiles_y)
    {
        by1 = static_cast<uint32_t>(config.tiles_y) - 1u;
    }

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            const uint32_t base_id = by * static_cast<uint32_t>(config.tiles_x) + bx;
            if (base_id < config.total_tiles)
            {
                fn(static_cast<uint16_t>(base_id));
            }
        }
    }
}

bool packet_header_valid(const wd_udp_tile_packet_decoded& header, size_t packet_size, uint16_t udp_payload_target,
                         const wd_server_config_payload& config) {
    if (header.session_id != config.session_id)
    {
        return false;
    }

    uint16_t tile_width = 0;
    uint16_t tile_height = 0;
    if (!tile_dimensions_from_header(header, tile_width, tile_height))
    {
        return false;
    }

    const uint16_t packet_tiles_x = wd_tiles_for_width_with_tile(config.width, tile_width);
    const uint16_t packet_tiles_y = wd_tiles_for_height_with_tile(config.height, tile_height);
    const uint32_t packet_total_tiles = static_cast<uint32_t>(packet_tiles_x) * static_cast<uint32_t>(packet_tiles_y);
    if (packet_tiles_x == 0 || packet_tiles_y == 0 || header.tile_id >= packet_total_tiles || packet_total_tiles > UINT16_MAX)
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

    if ((size_t)header.header_size + header.payload_size > packet_size)
    {
        return false;
    }

    if (header.compressed_tile_size == 0)
    {
        return false;
    }

    const size_t uncompressed_tile_bytes =
        static_cast<size_t>(tile_width) * static_cast<size_t>(tile_height) * WD_BYTES_PER_PIXEL;

    if (wd_tile_protocol_is_compressed(header.tile_protocol))
    {
        const size_t max_compressed_size = wd_zstd_compress_bound(uncompressed_tile_bytes);

        if (header.compressed_tile_size > max_compressed_size)
        {
            return false;
        }
    }
    else if (header.compressed_tile_size != uncompressed_tile_bytes)
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

    const uint64_t now_ns = wd_now_ns();
    state.summary_repair_loss_signal_until_ns.store(now_ns + WD_LINK_LARGE_SUMMARY_REPAIR_LOSS_SIGNAL_NS, std::memory_order_relaxed);

    state.stats.partial_tiles_timed_out.fetch_add(1, std::memory_order_relaxed);
    state.stats.partial_tile_missing_packets.fetch_add(missing_packets, std::memory_order_relaxed);
    reduce_tile_reassembly_timeout_after_loss(state);

    wd_udp_tile_packet_decoded header{};
    header.tile_id = entry.tile_id;
    header.tile_size = entry.tile_size;
    header.tile_generation = entry.generation;

    bool queued_any = false;
    for_each_base_tile_covered(header, state.config, [&](uint16_t base_id) {
        if (enqueue_retx_for_partial_timeout(state, base_id, entry.generation))
        {
            queued_any = true;
        }
        else
        {
            clear_retx_inflight(state, base_id, entry.generation);
        }
    });

    if (queued_any)
    {
        state.stats.partial_tile_retx_queued.fetch_add(1, std::memory_order_relaxed);
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

    if (!packet || packet_size < WD_UDP_TILE_HEADER_MIN_SIZE)
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    wd_udp_tile_packet_decoded header{};
    if (!wd_udp_tile_packet_decode(packet, packet_size, &header))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    uint16_t packet_tile_width = 0;
    uint16_t packet_tile_height = 0;
    if (!tile_dimensions_from_header(header, packet_tile_width, packet_tile_height))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    const size_t uncompressed_tile_bytes =
        static_cast<size_t>(packet_tile_width) * static_cast<size_t>(packet_tile_height) * WD_BYTES_PER_PIXEL;

    if (!wd_tile_protocol_is_compressed(header.tile_protocol))
    {
        header.compressed_tile_size = static_cast<uint16_t>(uncompressed_tile_bytes);
    }

    if (!packet_header_valid(header, packet_size, udp_payload_target, state.config))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    {
        std::lock_guard<std::mutex> lock(state.generation_mutex);

        bool old_generation = false;
        for_each_base_tile_covered(header, state.config, [&](uint16_t base_id) {
            if (base_id < state.displayed_generation.size() && header.tile_generation < state.displayed_generation[base_id])
            {
                old_generation = true;
            }
        });
        if (old_generation)
        {
            state.stats.udp_ignored_old_generation.fetch_add(1, std::memory_order_relaxed);
            return completed;
        }
    }

    const size_t entry_capacity = std::max<size_t>(state.config.total_tiles, 1u) * 4u;
    if (entries_.size() != entry_capacity)
    {
        entries_.assign(entry_capacity, Entry{});
    }

    Entry* entry_ptr = nullptr;
    for (Entry& candidate : entries_)
    {
        if (candidate.active && candidate.tile_id == header.tile_id && candidate.tile_size == header.tile_size)
        {
            entry_ptr = &candidate;
            break;
        }
    }
    if (!entry_ptr)
    {
        for (Entry& candidate : entries_)
        {
            if (!candidate.active)
            {
                entry_ptr = &candidate;
                break;
            }
        }
    }
    if (!entry_ptr)
    {
        expire_entry(state, entries_.front());
        entry_ptr = &entries_.front();
    }

    Entry& entry = *entry_ptr;

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
        entry.tile_size         = header.tile_size;
        entry.tile_width        = packet_tile_width;
        entry.tile_height       = packet_tile_height;
        entry.generation        = header.tile_generation;
        entry.tile_timestamp_ns = header.tile_timestamp_ns;
        entry.input_sequence    = header.input_sequence;
        entry.packet_count      = header.tile_pkt_count;
        entry.compressed_size   = header.compressed_tile_size;
        entry.compressed_payload = wd_tile_protocol_is_compressed(header.tile_protocol);
        entry.first_packet_ns   = now_ns;

        uint64_t retx_response_ns = 0;
        {
            std::lock_guard<std::mutex> lock(state.retx_mutex);

            const uint16_t base_id = tile_base_id_for_header(header, state.config);
            if (base_id < state.config.total_tiles && state.retx_last_requested_generation.size() == state.config.total_tiles &&
                state.retx_last_request_ns.size() == state.config.total_tiles &&
                state.retx_last_requested_generation[base_id] != 0 &&
                state.retx_last_requested_generation[base_id] <= entry.generation &&
                state.retx_last_request_ns[base_id] != 0 &&
                now_ns >= state.retx_last_request_ns[base_id])
            {
                retx_response_ns = now_ns - state.retx_last_request_ns[base_id];
                state.retx_inflight_grace_ns = clamp_retransmit_inflight_grace_ns(retx_response_ns + retx_response_ns / 2);
            }

            if (base_id < state.config.total_tiles && state.retx_inflight_generation.size() == state.config.total_tiles &&
                state.retx_inflight_since_ns.size() == state.config.total_tiles)
            {
                state.retx_inflight_generation[base_id] = entry.generation;
                state.retx_inflight_since_ns[base_id]  = entry.first_packet_ns;
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

    if (entry.packet_count != header.tile_pkt_count || entry.compressed_size != header.compressed_tile_size ||
        entry.compressed_payload != wd_tile_protocol_is_compressed(header.tile_protocol))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    const uint32_t offset = static_cast<uint32_t>(header.tile_pkt_id) * udp_payload_target;

    const uint8_t* payload = packet + header.header_size;

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

    completed.tile_bytes.assign(uncompressed_tile_bytes, 0);

    if (entry.compressed_payload)
    {
        const bool ok = wd_zstd_decompress(entry.compressed.data(), entry.compressed.size(), completed.tile_bytes.data(),
                                           completed.tile_bytes.size(), uncompressed_tile_bytes);

        if (!ok)
        {
            WD_LOG_ERROR("failed to decompress tile %u generation %llu", entry.tile_id,
                         static_cast<unsigned long long>(entry.generation));

            wd_udp_tile_packet_decoded clear_header{};
            clear_header.tile_id = entry.tile_id;
            clear_header.tile_size = entry.tile_size;
            for_each_base_tile_covered(clear_header, state.config, [&](uint16_t base_id) {
                clear_retx_inflight(state, base_id, entry.generation);
            });

            entry = Entry{};
            return completed;
        }
    }
    else
    {
        if (entry.compressed.size() != uncompressed_tile_bytes)
        {
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            wd_udp_tile_packet_decoded clear_header{};
            clear_header.tile_id = entry.tile_id;
            clear_header.tile_size = entry.tile_size;
            for_each_base_tile_covered(clear_header, state.config, [&](uint16_t base_id) {
                clear_retx_inflight(state, base_id, entry.generation);
            });
            entry = Entry{};
            return completed;
        }

        std::memcpy(completed.tile_bytes.data(), entry.compressed.data(), uncompressed_tile_bytes);
    }

    completed.valid                  = true;
    completed.tile_id                = entry.tile_id;
    completed.tile_width             = entry.tile_width;
    completed.tile_height            = entry.tile_height;
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

    wd_udp_tile_packet_decoded clear_header{};
    clear_header.tile_id = entry.tile_id;
    clear_header.tile_size = entry.tile_size;
    for_each_base_tile_covered(clear_header, state.config, [&](uint16_t base_id) {
        clear_retx_inflight(state, base_id, entry.generation);
    });

    entry = Entry{};

    return completed;
}

} // namespace waydisplay
