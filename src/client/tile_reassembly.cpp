#include "tile_reassembly.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace waydisplay {
namespace {

static_assert(WD_TILE_128x64 == 0 && WD_TILE_64x64 == 1 && WD_TILE_32x32 == 2 && WD_TILE_16x16 == 3,
              "tile-size codes must remain dense for indexed reassembly slots");

constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_MIN_NS     = WD_LINK_TILE_REASSEMBLY_MIN_NS;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_DEFAULT_NS = WD_LINK_TILE_REASSEMBLY_DEFAULT_NS;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_MAX_NS     = WD_LINK_TILE_REASSEMBLY_MAX_NS;
constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_SLACK_NS   = 50ull * 1000ull * 1000ull;
constexpr size_t   MAX_RECYCLED_ENTRIES               = 64;
constexpr size_t   MAX_RECYCLED_COMPLETED_BUFFERS     = 8;

size_t max_recycled_compressed_capacity() {
    constexpr size_t max_tile_bytes =
        static_cast<size_t>(WD_WIRE_TILE_MAX_WIDTH) * static_cast<size_t>(WD_WIRE_TILE_MAX_HEIGHT) * WD_BYTES_PER_PIXEL;
    return wd_zstd_compress_bound(max_tile_bytes);
}

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
    return std::max<uint64_t>(WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS, std::min<uint64_t>(WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS, ns));
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

    const double jittered_tile_ns =
        state.tile_reassembly_ewma_ns + 2.0 * state.tile_reassembly_deviation_ns + static_cast<double>(TILE_REASSEMBLY_TIMEOUT_SLACK_NS);
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

    const uint64_t old_ns    = current_tile_reassembly_timeout_ns(state);
    uint64_t       target_ns = old_ns * 3ull / 4ull;
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
        state.retx_inflight_since_ns[tile_id]   = 0;
    }
}

bool enqueue_retx_for_partial_timeout(ClientState& state, uint16_t tile_id, uint64_t generation) {
    std::scoped_lock generation_retx_lock(state.generation_mutex, state.retx_mutex);

    const uint16_t total_tiles = state.config.total_tiles;
    if (total_tiles == 0 || tile_id >= total_tiles || state.received_generation.size() != total_tiles)
    {
        return false;
    }

    if (state.received_generation[tile_id] >= generation)
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
        state.retx_inflight_since_ns[tile_id]   = 0;
    }

    return true;
}

bool tile_dimensions_from_header(const wd_udp_tile_packet_decoded& header, uint16_t& tile_width, uint16_t& tile_height) {
    return wd_tile_dimensions_for_size_code(header.tile_size, &tile_width, &tile_height);
}

template <typename Fn>
void for_each_base_tile_covered(const wd_udp_tile_packet_decoded& header, const wd_server_config_payload& config, Fn&& fn) {
    uint16_t tile_width  = 0;
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

void clear_entry_retx_inflight(ClientState& state, uint16_t tile_id, uint8_t tile_size, uint64_t generation) {
    wd_udp_tile_packet_decoded header{};
    header.tile_id   = tile_id;
    header.tile_size = tile_size;
    for_each_base_tile_covered(header, state.config, [&](uint16_t base_id) { clear_retx_inflight(state, base_id, generation); });
}

TilePacketValidationResult packet_header_validation_result(const wd_udp_tile_packet_decoded& header, size_t packet_size,
                                                           uint16_t udp_payload_target, const wd_server_config_payload& config) {
    if (header.session_id != config.session_id || header.connection_token != config.connection_token) [[unlikely]]
    {
        return TilePacketValidationResult::Identity;
    }

    uint16_t tile_width  = 0;
    uint16_t tile_height = 0;
    if (!tile_dimensions_from_header(header, tile_width, tile_height)) [[unlikely]]
    {
        return TilePacketValidationResult::Geometry;
    }

    const uint16_t packet_tiles_x     = wd_tiles_for_width_with_tile(config.width, tile_width);
    const uint16_t packet_tiles_y     = wd_tiles_for_height_with_tile(config.height, tile_height);
    const uint32_t packet_total_tiles = static_cast<uint32_t>(packet_tiles_x) * static_cast<uint32_t>(packet_tiles_y);
    if (packet_tiles_x == 0 || packet_tiles_y == 0 || header.tile_id >= packet_total_tiles || packet_total_tiles > UINT16_MAX) [[unlikely]]
    {
        return TilePacketValidationResult::Geometry;
    }

    if (header.tile_pkt_count == 0 || header.tile_pkt_id >= header.tile_pkt_count || header.payload_size == 0 ||
        header.tile_payload_size == 0) [[unlikely]]
    {
        return TilePacketValidationResult::Fragment;
    }

    const size_t uncompressed_tile_bytes = static_cast<size_t>(tile_width) * static_cast<size_t>(tile_height) * WD_BYTES_PER_PIXEL;

    if (wd_udp_tile_packet_is_compressed(&header))
    {
        if (header.tile_payload_size > wd_zstd_compress_bound(uncompressed_tile_bytes))
        {
            return TilePacketValidationResult::Fragment;
        }
    }
    else if (header.tile_payload_size != uncompressed_tile_bytes)
    {
        return TilePacketValidationResult::Fragment;
    }

    if (!wd_udp_tile_fragment_layout_valid(&header, packet_size, udp_payload_target, header.tile_payload_size)) [[unlikely]]
    {
        return TilePacketValidationResult::Fragment;
    }
    return TilePacketValidationResult::Valid;
}

} // namespace

TilePacketValidationResult validate_tile_packet_header(const wd_udp_tile_packet_decoded& header, size_t packet_size,
                                                       uint16_t udp_payload_target, const wd_server_config_payload& config) {
    return packet_header_validation_result(header, packet_size, udp_payload_target, config);
}

TileReassembler::TileReassembler(size_t max_active_entries, size_t max_active_payload_bytes)
    : max_active_entries_(std::max<size_t>(1, max_active_entries)),
      max_active_payload_bytes_(std::max<size_t>(1, max_active_payload_bytes)) {
}

void TileReassembler::reset() {
    while (!active_entries_.empty())
    {
        remove_entry(active_entries_.size() - 1u);
    }
    for (std::vector<uint32_t>& slots : entry_slots_by_size_)
    {
        slots.clear();
    }
    entry_frame_width_    = 0;
    entry_frame_height_   = 0;
    active_payload_bytes_ = 0;
}

bool TileReassembler::configure_entry_slots(const wd_server_config_payload& config) {
    if (entry_frame_width_ == config.width && entry_frame_height_ == config.height && entry_frame_width_ != 0) [[likely]]
    {
        return true;
    }

    std::array<std::vector<uint32_t>, 4> new_slots;
    size_t                               total_slot_count = 0;
    for (uint8_t tile_size = WD_TILE_128x64; tile_size <= WD_TILE_16x16; ++tile_size)
    {
        uint16_t tile_width  = 0;
        uint16_t tile_height = 0;
        if (!wd_tile_dimensions_for_size_code(tile_size, &tile_width, &tile_height)) [[unlikely]]
        {
            return false;
        }

        const uint32_t tiles_x     = wd_tiles_for_width_with_tile(config.width, tile_width);
        const uint32_t tiles_y     = wd_tiles_for_height_with_tile(config.height, tile_height);
        const uint32_t total_tiles = tiles_x * tiles_y;
        if (tiles_x == 0 || tiles_y == 0 || total_tiles > UINT16_MAX) [[unlikely]]
        {
            return false;
        }
        new_slots[tile_size].assign(total_tiles, INVALID_ENTRY_INDEX);
        total_slot_count += total_tiles;
    }

    while (!active_entries_.empty())
    {
        remove_entry(active_entries_.size() - 1u);
    }
    entry_slots_by_size_ = std::move(new_slots);
    active_entries_.reserve(std::min<size_t>(total_slot_count, 256));
    entry_frame_width_  = config.width;
    entry_frame_height_ = config.height;
    return true;
}

size_t TileReassembler::find_entry_index(uint8_t tile_size, uint16_t tile_id) const {
    if (tile_size >= entry_slots_by_size_.size() || tile_id >= entry_slots_by_size_[tile_size].size()) [[unlikely]]
    {
        return SIZE_MAX;
    }
    const uint32_t entry_index = entry_slots_by_size_[tile_size][tile_id];
    return entry_index == INVALID_ENTRY_INDEX ? SIZE_MAX : static_cast<size_t>(entry_index);
}

size_t TileReassembler::activate_entry(uint8_t tile_size, uint16_t tile_id) {
    if (tile_size >= entry_slots_by_size_.size() || tile_id >= entry_slots_by_size_[tile_size].size() ||
        active_entries_.size() >= INVALID_ENTRY_INDEX)
    {
        return SIZE_MAX;
    }

    const size_t entry_index = active_entries_.size();
    if (recycled_entries_.empty())
    {
        active_entries_.emplace_back();
    }
    else
    {
        active_entries_.push_back(std::move(recycled_entries_.back()));
        recycled_entries_.pop_back();
    }

    Entry& entry             = active_entries_.back();
    entry.tile_id            = tile_id;
    entry.tile_size          = tile_size;
    entry.tile_width         = 0;
    entry.tile_height        = 0;
    entry.generation         = 0;
    entry.content_epoch      = 0;
    entry.input_sequence     = 0;
    entry.packet_count       = 0;
    entry.compressed_size    = 0;
    entry.compressed_payload = true;
    entry.first_packet_ns    = 0;
    entry.compressed.clear();
    entry.received_bitmap.fill(0);
    entry.received_count = 0;

    entry_slots_by_size_[tile_size][tile_id] = static_cast<uint32_t>(entry_index);
    return entry_index;
}

void TileReassembler::recycle_entry(Entry&& entry) {
    entry.compressed.clear();
    entry.received_bitmap.fill(0);
    if (entry.compressed.capacity() > max_recycled_compressed_capacity())
    {
        std::vector<uint8_t>().swap(entry.compressed);
    }
    if (recycled_entries_.size() < MAX_RECYCLED_ENTRIES)
    {
        recycled_entries_.push_back(std::move(entry));
    }
}

void TileReassembler::remove_entry(size_t entry_index) {
    if (entry_index >= active_entries_.size())
    {
        return;
    }

    Entry retired = std::move(active_entries_[entry_index]);
    if (active_payload_bytes_ >= retired.compressed.size())
    {
        active_payload_bytes_ -= retired.compressed.size();
    }
    else
    {
        active_payload_bytes_ = 0;
    }
    if (retired.tile_size < entry_slots_by_size_.size() && retired.tile_id < entry_slots_by_size_[retired.tile_size].size())
    {
        entry_slots_by_size_[retired.tile_size][retired.tile_id] = INVALID_ENTRY_INDEX;
    }

    const size_t last_index = active_entries_.size() - 1u;
    if (entry_index != last_index)
    {
        active_entries_[entry_index]                         = std::move(active_entries_[last_index]);
        const Entry& moved                                   = active_entries_[entry_index];
        entry_slots_by_size_[moved.tile_size][moved.tile_id] = static_cast<uint32_t>(entry_index);
    }
    active_entries_.pop_back();
    recycle_entry(std::move(retired));
}

std::vector<uint8_t> TileReassembler::acquire_completed_tile_buffer(size_t size) {
    std::vector<uint8_t> buffer;
    if (!recycled_completed_buffers_.empty())
    {
        buffer = std::move(recycled_completed_buffers_.back());
        recycled_completed_buffers_.pop_back();
    }
    buffer.resize(size);
    std::fill(buffer.begin(), buffer.end(), 0);
    return buffer;
}

void TileReassembler::recycle_completed_tile_buffer(std::vector<uint8_t>&& buffer) {
    buffer.clear();
    constexpr size_t max_tile_bytes =
        static_cast<size_t>(WD_WIRE_TILE_MAX_WIDTH) * static_cast<size_t>(WD_WIRE_TILE_MAX_HEIGHT) * WD_BYTES_PER_PIXEL;
    if (buffer.capacity() > max_tile_bytes || recycled_completed_buffers_.size() >= MAX_RECYCLED_COMPLETED_BUFFERS)
    {
        return;
    }
    recycled_completed_buffers_.push_back(std::move(buffer));
}

size_t TileReassembler::active_entry_count() const {
    return active_entries_.size();
}

size_t TileReassembler::recycled_entry_count() const {
    return recycled_entries_.size();
}

size_t TileReassembler::recycled_completed_buffer_count() const {
    return recycled_completed_buffers_.size();
}

size_t TileReassembler::slot_count() const {
    size_t total = 0;
    for (const std::vector<uint32_t>& slots : entry_slots_by_size_)
    {
        total += slots.size();
    }
    return total;
}

size_t TileReassembler::active_payload_bytes() const {
    return active_payload_bytes_;
}

uint64_t TileReassembler::count_missing_packets(const Entry& entry) {
    uint64_t missing = 0;
    for (uint16_t packet_id = 0; packet_id < entry.packet_count; ++packet_id)
    {
        const uint64_t mask = 1ull << (packet_id & 63u);
        if ((entry.received_bitmap[packet_id >> 6u] & mask) == 0)
        {
            missing++;
        }
    }
    return missing;
}

void TileReassembler::expire_entry(ClientState& state, size_t entry_index) {
    if (entry_index >= active_entries_.size())
    {
        return;
    }

    const Entry&   entry           = active_entries_[entry_index];
    const uint64_t missing_packets = count_missing_packets(entry);

    state.stats.partial_tiles_timed_out.fetch_add(1, std::memory_order_relaxed);
    state.stats.partial_tile_missing_packets.fetch_add(missing_packets, std::memory_order_relaxed);
    reduce_tile_reassembly_timeout_after_loss(state);
    queue_entry_repair(state, entry);
    remove_entry(entry_index);
}

void TileReassembler::queue_entry_repair(ClientState& state, const Entry& entry) {
    const uint64_t now_ns = wd_now_ns();
    state.summary_repair_loss_signal_until_ns.store(now_ns + WD_LINK_LARGE_SUMMARY_REPAIR_LOSS_SIGNAL_NS, std::memory_order_relaxed);

    wd_udp_tile_packet_decoded header{};
    header.tile_id         = entry.tile_id;
    header.tile_size       = entry.tile_size;
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
}

void TileReassembler::evict_entry_for_budget(ClientState& state, size_t entry_index) {
    if (entry_index >= active_entries_.size())
    {
        return;
    }
    queue_entry_repair(state, active_entries_[entry_index]);
    state.stats.reassembly_budget_evictions.fetch_add(1, std::memory_order_relaxed);
    remove_entry(entry_index);
}

bool TileReassembler::ensure_budget(ClientState& state, size_t payload_size) {
    if (payload_size > max_active_payload_bytes_) [[unlikely]]
    {
        state.stats.reassembly_budget_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    while (!active_entries_.empty() &&
           (active_entries_.size() >= max_active_entries_ || active_payload_bytes_ + payload_size > max_active_payload_bytes_))
    {
        size_t oldest_index = 0;
        for (size_t i = 1; i < active_entries_.size(); ++i)
        {
            if (active_entries_[i].first_packet_ns < active_entries_[oldest_index].first_packet_ns)
            {
                oldest_index = i;
            }
        }
        evict_entry_for_budget(state, oldest_index);
    }
    if (active_entries_.size() >= max_active_entries_ || active_payload_bytes_ + payload_size > max_active_payload_bytes_) [[unlikely]]
    {
        state.stats.reassembly_budget_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

uint64_t TileReassembler::next_expiry_deadline_ns(const ClientState& state) const {
    const uint64_t timeout_ns  = current_tile_reassembly_timeout_ns(state);
    uint64_t       deadline_ns = 0;
    for (const Entry& entry : active_entries_)
    {
        if (entry.first_packet_ns == 0)
        {
            continue;
        }
        const uint64_t candidate = entry.first_packet_ns > UINT64_MAX - timeout_ns ? UINT64_MAX : entry.first_packet_ns + timeout_ns;
        if (deadline_ns == 0 || candidate < deadline_ns)
        {
            deadline_ns = candidate;
        }
    }
    return deadline_ns;
}

void TileReassembler::expire_stale_entries(ClientState& state) {
    const uint64_t now_ns = wd_now_ns();

    size_t entry_index = 0;
    while (entry_index < active_entries_.size())
    {
        const Entry& entry = active_entries_[entry_index];
        if (entry.first_packet_ns != 0 && now_ns - entry.first_packet_ns > current_tile_reassembly_timeout_ns(state))
        {
            expire_entry(state, entry_index);
        }
        else
        {
            ++entry_index;
        }
    }
}

CompletedTile TileReassembler::process_udp_packet(ClientState& state, const uint8_t* packet, size_t packet_size) {
    CompletedTile completed{};

    if (!packet || packet_size < WD_UDP_TILE_HEADER_MIN_SIZE) [[unlikely]]
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_invalid_short.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    wd_udp_tile_packet_decoded header{};
    if (!wd_udp_tile_packet_decode(packet, packet_size, &header)) [[unlikely]]
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_invalid_header.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    uint16_t packet_tile_width  = 0;
    uint16_t packet_tile_height = 0;
    if (!tile_dimensions_from_header(header, packet_tile_width, packet_tile_height)) [[unlikely]]
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_invalid_geometry.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    const size_t uncompressed_tile_bytes =
        static_cast<size_t>(packet_tile_width) * static_cast<size_t>(packet_tile_height) * WD_BYTES_PER_PIXEL;

    const TilePacketValidationResult validation = validate_tile_packet_header(header, packet_size, udp_payload_target, state.config);
    if (validation != TilePacketValidationResult::Valid) [[unlikely]]
    {
        if (validation == TilePacketValidationResult::Identity)
        {
            state.stats.udp_ignored_stale_session.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            if (validation == TilePacketValidationResult::Geometry)
            {
                state.stats.udp_invalid_geometry.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                state.stats.udp_invalid_fragment.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return completed;
    }

    {
        std::lock_guard<std::mutex> lock(state.generation_mutex);

        bool covered_any   = false;
        bool covered_newer = false;
        bool covered_older = false;
        for_each_base_tile_covered(header, state.config, [&](uint16_t base_id) {
            if (base_id >= state.received_generation.size())
            {
                return;
            }
            covered_any             = true;
            const uint64_t received = state.received_generation[base_id];
            covered_newer           = covered_newer || received > header.tile_generation;
            covered_older           = covered_older || received < header.tile_generation;
        });
        if (!covered_any || covered_newer || !covered_older)
        {
            state.stats.udp_ignored_old_generation.fetch_add(1, std::memory_order_relaxed);
            return completed;
        }
    }

    if (!configure_entry_slots(state.config)) [[unlikely]]
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    size_t         entry_index = find_entry_index(header.tile_size, header.tile_id);
    const uint64_t now_ns      = wd_now_ns();

    if (entry_index != SIZE_MAX && active_entries_[entry_index].content_epoch == header.content_epoch &&
        header.tile_generation < active_entries_[entry_index].generation)
    {
        state.stats.udp_ignored_old_generation.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    if (entry_index != SIZE_MAX && active_entries_[entry_index].content_epoch == header.content_epoch &&
        active_entries_[entry_index].generation == header.tile_generation && active_entries_[entry_index].first_packet_ns != 0 &&
        now_ns - active_entries_[entry_index].first_packet_ns > current_tile_reassembly_timeout_ns(state))
    {
        expire_entry(state, entry_index);
        entry_index = SIZE_MAX;
    }

    if (entry_index == SIZE_MAX || active_entries_[entry_index].content_epoch != header.content_epoch ||
        active_entries_[entry_index].generation != header.tile_generation)
    {
        if (entry_index != SIZE_MAX)
        {
            const Entry& old_entry = active_entries_[entry_index];
            clear_entry_retx_inflight(state, old_entry.tile_id, old_entry.tile_size, old_entry.generation);
            remove_entry(entry_index);
        }

        if (!ensure_budget(state, header.tile_payload_size)) [[unlikely]]
        {
            return completed;
        }
        entry_index = activate_entry(header.tile_size, header.tile_id);
        if (entry_index == SIZE_MAX) [[unlikely]]
        {
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            return completed;
        }

        Entry& entry                       = active_entries_[entry_index];
        entry.tile_width                   = packet_tile_width;
        entry.tile_height                  = packet_tile_height;
        entry.generation                   = header.tile_generation;
        entry.content_epoch                = header.content_epoch;
        entry.input_sequence               = header.tile_pkt_id == 0 ? header.input_sequence : 0;
        entry.first_fragment_metadata_seen = header.tile_pkt_id == 0;
        entry.packet_count                 = header.tile_pkt_count;
        entry.compressed_size              = header.tile_payload_size;
        entry.compressed_payload           = wd_udp_tile_packet_is_compressed(&header);
        entry.first_packet_ns              = now_ns;

        uint64_t retx_response_ns = 0;
        {
            std::lock_guard<std::mutex> lock(state.retx_mutex);

            for_each_base_tile_covered(header, state.config, [&](uint16_t base_id) {
                if (base_id >= state.config.total_tiles)
                {
                    return;
                }

                if (state.retx_last_requested_generation.size() == state.config.total_tiles &&
                    state.retx_last_request_ns.size() == state.config.total_tiles && state.retx_last_requested_generation[base_id] != 0 &&
                    state.retx_last_requested_generation[base_id] <= entry.generation && state.retx_last_request_ns[base_id] != 0 &&
                    now_ns >= state.retx_last_request_ns[base_id])
                {
                    const uint64_t sample_ns = now_ns - state.retx_last_request_ns[base_id];
                    if (retx_response_ns == 0 || sample_ns < retx_response_ns)
                    {
                        retx_response_ns = sample_ns;
                    }
                }

                if (state.retx_inflight_generation.size() == state.config.total_tiles &&
                    state.retx_inflight_since_ns.size() == state.config.total_tiles)
                {
                    state.retx_inflight_generation[base_id] = entry.generation;
                    state.retx_inflight_since_ns[base_id]   = entry.first_packet_ns;
                }
            });

            if (retx_response_ns != 0)
            {
                state.retx_inflight_grace_ns = clamp_retransmit_inflight_grace_ns(retx_response_ns + retx_response_ns / 2);
            }
        }

        if (retx_response_ns != 0)
        {
            state.stats.retx_response_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.retx_response_sum_ns.fetch_add(retx_response_ns, std::memory_order_relaxed);
            update_tile_reassembly_timeout(state, retx_response_ns, true);
        }

        entry.compressed.assign(header.tile_payload_size, 0);
        active_payload_bytes_ += entry.compressed.size();
        entry.received_bitmap.fill(0);
        entry.received_count = 0;
    }

    Entry& entry = active_entries_[entry_index];
    if (header.tile_pkt_id == 0)
    {
        if (entry.first_fragment_metadata_seen && entry.input_sequence != header.input_sequence) [[unlikely]]
        {
            state.stats.tile_fragment_conflicts.fetch_add(1, std::memory_order_relaxed);
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            queue_entry_repair(state, entry);
            remove_entry(entry_index);
            return completed;
        }
        entry.first_fragment_metadata_seen = true;
        entry.input_sequence               = header.input_sequence;
    }
    if (entry.packet_count != header.tile_pkt_count || entry.compressed_size != header.tile_payload_size ||
        entry.compressed_payload != wd_udp_tile_packet_is_compressed(&header)) [[unlikely]]
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return completed;
    }

    const uint32_t offset  = static_cast<uint32_t>(header.tile_pkt_id) * udp_payload_target;
    const uint8_t* payload = packet + header.header_size;

    const size_t   bitmap_word = header.tile_pkt_id >> 6u;
    const uint64_t bitmap_mask = 1ull << (header.tile_pkt_id & 63u);
    if ((entry.received_bitmap[bitmap_word] & bitmap_mask) == 0) [[likely]]
    {
        std::memcpy(entry.compressed.data() + offset, payload, header.payload_size);
        entry.received_bitmap[bitmap_word] |= bitmap_mask;
        entry.received_count++;
    }
    else if (std::memcmp(entry.compressed.data() + offset, payload, header.payload_size) != 0)
    {
        state.stats.tile_fragment_conflicts.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        queue_entry_repair(state, entry);
        remove_entry(entry_index);
        return completed;
    }

    if (entry.received_count != entry.packet_count)
    {
        return completed;
    }

    completed.tile_bytes = acquire_completed_tile_buffer(uncompressed_tile_bytes);

    if (entry.compressed_payload)
    {
        const bool ok = wd_zstd_decompress(entry.compressed.data(), entry.compressed.size(), completed.tile_bytes.data(),
                                           completed.tile_bytes.size(), uncompressed_tile_bytes);

        if (!ok) [[unlikely]]
        {
            WD_LOG_ERROR("failed to decompress tile %u generation %llu", entry.tile_id, static_cast<unsigned long long>(entry.generation));
            state.stats.tile_decompress_failures.fetch_add(1, std::memory_order_relaxed);
            queue_entry_repair(state, entry);
            recycle_completed_tile_buffer(std::move(completed.tile_bytes));
            remove_entry(entry_index);
            return completed;
        }
    }
    else
    {
        if (entry.compressed.size() != uncompressed_tile_bytes)
        {
            state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
            queue_entry_repair(state, entry);
            recycle_completed_tile_buffer(std::move(completed.tile_bytes));
            remove_entry(entry_index);
            return completed;
        }

        std::memcpy(completed.tile_bytes.data(), entry.compressed.data(), uncompressed_tile_bytes);
    }

    completed.valid                  = true;
    completed.tile_id                = entry.tile_id;
    completed.tile_width             = entry.tile_width;
    completed.tile_height            = entry.tile_height;
    completed.generation             = entry.generation;
    completed.content_epoch          = entry.content_epoch;
    completed.input_sequence         = entry.input_sequence;
    completed.first_packet_ns        = entry.first_packet_ns;
    completed.completed_timestamp_ns = wd_now_ns();
    completed.compressed_size        = entry.compressed_size;
    completed.packet_count           = entry.packet_count;

    if (completed.completed_timestamp_ns >= completed.first_packet_ns)
    {
        update_tile_reassembly_timeout(state, completed.completed_timestamp_ns - completed.first_packet_ns, false);
    }

    clear_entry_retx_inflight(state, entry.tile_id, entry.tile_size, entry.generation);
    remove_entry(entry_index);

    return completed;
}

} // namespace waydisplay
