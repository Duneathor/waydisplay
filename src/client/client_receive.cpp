#include "client_receive.hpp"

#include "client_async_udp.hpp"
#include "client_net.hpp"
#include "content_order.hpp"
#include "client_state.hpp"
#include "client_telemetry.hpp"
#include "render_planning.hpp"
#include "tile_reassembly.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace waydisplay {

struct ClientReceiveState {
    TileReassembler reassembler;
    uint64_t        observed_config_generation = 0;
};

namespace {

bool blit_tile_xrgb8888(ClientState& state, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                        const std::vector<uint8_t>& tile_bytes, ClientDirtyRect& dirty_rect) {
    const uint16_t tiles_x     = wd_tiles_for_width_with_tile(state.config.width, tile_width);
    const uint16_t tiles_y     = wd_tiles_for_height_with_tile(state.config.height, tile_height);
    const uint32_t total_tiles = static_cast<uint32_t>(tiles_x) * static_cast<uint32_t>(tiles_y);
    if (tile_width == 0 || tile_height == 0 || tiles_x == 0 || tiles_y == 0 || tile_id >= total_tiles)
    {
        return false;
    }

    const uint32_t tile_x        = tile_id % tiles_x;
    const uint32_t tile_y        = tile_id / tiles_x;
    const uint32_t dst_x         = tile_x * tile_width;
    const uint32_t dst_y         = tile_y * tile_height;
    const size_t   expected_size = static_cast<size_t>(tile_width) * static_cast<size_t>(tile_height) * WD_BYTES_PER_PIXEL;

    if (tile_bytes.size() < expected_size || dst_x >= state.config.width || dst_y >= state.config.height)
    {
        return false;
    }

    const uint32_t visible_width  = std::min<uint32_t>(tile_width, state.config.width - dst_x);
    const uint32_t visible_height = std::min<uint32_t>(tile_height, state.config.height - dst_y);

    dirty_rect.x = static_cast<uint16_t>(dst_x);
    dirty_rect.y = static_cast<uint16_t>(dst_y);
    dirty_rect.w = static_cast<uint16_t>(visible_width);
    dirty_rect.h = static_cast<uint16_t>(visible_height);

    for (uint32_t y = 0; y < visible_height; ++y)
    {
        const uint8_t* src = tile_bytes.data() + static_cast<size_t>(y) * tile_width * WD_BYTES_PER_PIXEL;
        uint32_t*      dst = state.framebuffer.data() + static_cast<size_t>(dst_y + y) * state.config.width + dst_x;

        std::memcpy(dst, src, static_cast<size_t>(visible_width) * WD_BYTES_PER_PIXEL);
    }

    return true;
}

void clear_completed_repair_tracking_locked(ClientState& state, uint32_t base_id) {
    if (base_id >= state.received_generation.size())
    {
        return;
    }

    const uint64_t received = state.received_generation[base_id];
    if (base_id < state.retx_queued_generation.size() && state.retx_queued_generation[base_id] != 0 &&
        state.retx_queued_generation[base_id] <= received)
    {
        state.retx_queued_generation[base_id] = 0;
    }
    if (base_id < state.retx_inflight_generation.size() && state.retx_inflight_generation[base_id] != 0 &&
        state.retx_inflight_generation[base_id] <= received)
    {
        state.retx_inflight_generation[base_id] = 0;
        if (base_id < state.retx_inflight_since_ns.size())
        {
            state.retx_inflight_since_ns[base_id] = 0;
        }
    }
    if (base_id < state.retx_last_requested_generation.size() && state.retx_last_requested_generation[base_id] != 0 &&
        state.retx_last_requested_generation[base_id] <= received)
    {
        state.retx_last_requested_generation[base_id] = 0;
        if (base_id < state.retx_last_request_ns.size())
        {
            state.retx_last_request_ns[base_id] = 0;
        }
    }
    if (base_id < state.retx_summary_pending_generation.size() && state.retx_summary_pending_generation[base_id] != 0 &&
        state.retx_summary_pending_generation[base_id] <= received)
    {
        (void)summary_pending_index_remove(state.retx_summary_pending_tiles, state.retx_summary_pending_position,
                                           static_cast<uint16_t>(base_id));
        state.retx_summary_pending_generation[base_id] = 0;
        if (base_id < state.retx_summary_pending_since_ns.size())
        {
            state.retx_summary_pending_since_ns[base_id] = 0;
        }
        state.retx_summary_pending_count = static_cast<uint32_t>(state.retx_summary_pending_tiles.size());
        state.stats.summary_retx_tiles_stale_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

template <typename Fn> void for_each_completed_base_tile(const ClientState& state, const CompletedTile& completed, Fn&& fn) {
    if (state.config.tile_width == 0 || state.config.tile_height == 0 || completed.tile_width == 0 || completed.tile_height == 0)
    {
        return;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(state.config.width, completed.tile_width);
    if (tiles_x == 0)
    {
        return;
    }

    const uint32_t x = wd_tile_start_x_for_tile(completed.tile_id, tiles_x, completed.tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(completed.tile_id, tiles_x, completed.tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(state.config.width, completed.tile_id, tiles_x, completed.tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(state.config.height, completed.tile_id, tiles_x, completed.tile_height);
    if (w == 0 || h == 0)
    {
        return;
    }

    const uint32_t bx0 = x / state.config.tile_width;
    const uint32_t by0 = y / state.config.tile_height;
    const uint32_t bx1 = std::min<uint32_t>((x + w - 1u) / state.config.tile_width, static_cast<uint32_t>(state.config.tiles_x) - 1u);
    const uint32_t by1 = std::min<uint32_t>((y + h - 1u) / state.config.tile_height, static_cast<uint32_t>(state.config.tiles_y) - 1u);

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            const uint32_t base_id = by * static_cast<uint32_t>(state.config.tiles_x) + bx;
            if (base_id < state.config.total_tiles)
            {
                fn(static_cast<uint16_t>(base_id));
            }
        }
    }
}

void mark_completed_base_generations(ClientState& state, const CompletedTile& completed) {
    for_each_completed_base_tile(state, completed, [&](uint16_t base_id) {
        if (base_id < state.received_generation.size() && completed.generation > state.received_generation[base_id])
        {
            state.received_generation[base_id] = completed.generation;
        }
        if (base_id < state.pending_present_generation.size() && completed.generation > state.pending_present_generation[base_id])
        {
            state.pending_present_generation[base_id] = completed.generation;
        }
        clear_completed_repair_tracking_locked(state, base_id);
    });
}

bool client_has_pending_server_config(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.config_mutex);
    return state.pending_config_valid;
}

bool process_udp_datagram(ClientState& state, TileReassembler& reassembler, const uint8_t* packet, size_t packet_size) {
    if (!packet || packet_size < WD_UDP_TILE_HEADER_MIN_SIZE)
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_invalid_short.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    wd_udp_tile_packet_decoded udp_header{};
    if (!wd_udp_tile_packet_decode(packet, packet_size, &udp_header))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_invalid_header.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (udp_header.tile_id == WD_UDP_TILE_ID_MTU_PROBE || udp_header.tile_id == WD_UDP_TILE_ID_THROUGHPUT_PROBE)
    {
        state.stats.udp_ignored_probe.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    uint8_t  session_id       = 0;
    uint64_t connection_token = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id       = state.config.session_id;
        connection_token = state.config.connection_token;
    }

    if (session_id == 0 || udp_header.session_id != session_id || udp_header.connection_token != connection_token)
    {
        state.stats.udp_ignored_stale_session.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const ClientContentEpochDecision content_decision =
        client_accept_content_epoch(state, udp_header.content_epoch, WD_CLIENT_CONTENT_OWNER_TILES);
    if (content_decision == ClientContentEpochDecision::Stale)
    {
        state.stats.udp_ignored_stale_epoch.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (content_decision == ClientContentEpochDecision::Advanced)
    {
        reassembler.reset();
    }

    const uint64_t packet_rx_ns = wd_now_ns();
    const uint64_t prev_rx_ns   = state.stats.last_udp_packet_rx_ns.exchange(packet_rx_ns, std::memory_order_relaxed);
    if (prev_rx_ns != 0 && packet_rx_ns >= prev_rx_ns)
    {
        const uint64_t interarrival_ns = packet_rx_ns - prev_rx_ns;
        state.stats.udp_interarrival_samples.fetch_add(1, std::memory_order_relaxed);
        state.stats.udp_interarrival_sum_ns.fetch_add(interarrival_ns, std::memory_order_relaxed);
        record_atomic_max(state.stats.udp_interarrival_max_ns, interarrival_ns);

        const uint64_t prev_interarrival_ns = state.stats.last_udp_interarrival_ns.exchange(interarrival_ns, std::memory_order_relaxed);
        if (prev_interarrival_ns != 0)
        {
            const uint64_t jitter_ns =
                interarrival_ns > prev_interarrival_ns ? interarrival_ns - prev_interarrival_ns : prev_interarrival_ns - interarrival_ns;
            state.stats.udp_interarrival_jitter_samples.fetch_add(1, std::memory_order_relaxed);
            state.stats.udp_interarrival_jitter_sum_ns.fetch_add(jitter_ns, std::memory_order_relaxed);
        }
    }

    state.stats.udp_packets_rx.fetch_add(1, std::memory_order_relaxed);
    state.stats.udp_bytes_rx.fetch_add(static_cast<uint64_t>(packet_size), std::memory_order_relaxed);

    CompletedTile completed = reassembler.process_udp_packet(state, packet, packet_size);

    if (!completed.valid)
    {
        return true;
    }

    ClientDirtyRect dirty_rect{};
    bool            wake_render = false;
    {
        std::lock_guard<std::mutex> content_lock(state.remote_content_mutex);
        if (completed.content_epoch != state.remote_content_epoch || state.remote_content_owner != WD_CLIENT_CONTENT_OWNER_TILES)
        {
            reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));
            state.stats.udp_ignored_stale_session.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        {
            std::lock_guard<std::mutex> framebuffer_lock(state.framebuffer_mutex);
            if (!blit_tile_xrgb8888(state, completed.tile_id, completed.tile_width, completed.tile_height, completed.tile_bytes,
                                    dirty_rect))
            {
                reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));
                state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
                state.stats.udp_invalid_blit.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));

        {
            std::lock_guard<std::mutex> dirty_lock(state.dirty_rect_mutex);
            const bool                  dirty_was_empty = state.pending_dirty_tiles.dirty_tile_count() == 0;
            if (!state.pending_dirty_tiles.mark_rect(dirty_rect))
            {
                state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
                state.stats.udp_invalid_dirty_grid.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            state.pending_dirty_rect_count.store(state.pending_dirty_tiles.dirty_tile_count(), std::memory_order_release);
            state.pending_dirty_epoch = wd_client_stream_ownership_snapshot(&state.stream_ownership).epoch;
            wake_render               = dirty_was_empty;
        }

        {
            std::scoped_lock generation_retx_lock(state.generation_mutex, state.retx_mutex);
            mark_completed_base_generations(state, completed);
        }

        if (completed.completed_timestamp_ns != 0)
        {
            std::lock_guard<std::mutex> present_lock(state.present_mutex);
            uint64_t                    completion_id = state.next_tile_completion_id++;
            if (completion_id == 0)
            {
                completion_id = state.next_tile_completion_id++;
            }
            for_each_completed_base_tile(state, completed, [&](uint16_t base_id) {
                if (base_id >= state.pending_tile_telemetry.size())
                {
                    return;
                }
                ClientPendingTileTelemetry& telemetry = state.pending_tile_telemetry[base_id];
                if (completed.generation >= telemetry.generation)
                {
                    telemetry.completion_id  = completion_id;
                    telemetry.content_epoch  = completed.content_epoch;
                    telemetry.generation     = completed.generation;
                    telemetry.completed_ns   = completed.completed_timestamp_ns;
                    telemetry.input_sequence = completed.input_sequence;
                }
            });
        }
    }

    if (completed.first_packet_ns != 0 && completed.completed_timestamp_ns >= completed.first_packet_ns)
    {
        state.stats.tile_assembly_samples.fetch_add(1, std::memory_order_relaxed);
        state.stats.tile_assembly_sum_ns.fetch_add(completed.completed_timestamp_ns - completed.first_packet_ns, std::memory_order_relaxed);
    }

    state.stats.udp_completed_compressed_bytes.fetch_add(completed.compressed_size, std::memory_order_relaxed);
    state.stats.udp_completed_packets.fetch_add(completed.packet_count, std::memory_order_relaxed);
    state.stats.udp_tiles_completed.fetch_add(1, std::memory_order_relaxed);
    if (wake_render)
    {
        state.render_wake.signal();
    }
    return true;
}

struct AsyncUdpDrainContext {
    ClientState*     state       = nullptr;
    TileReassembler* reassembler = nullptr;
};

bool handle_async_udp_packet(void* userdata, const uint8_t* packet, size_t packet_size) {
    auto* ctx = static_cast<AsyncUdpDrainContext*>(userdata);
    if (!ctx || !ctx->state || !ctx->reassembler)
    {
        return false;
    }
    if (client_has_pending_server_config(*ctx->state))
    {
        return true;
    }
    return process_udp_datagram(*ctx->state, *ctx->reassembler, packet, packet_size);
}

bool drain_udp(ClientState& state, TileReassembler& reassembler) {
    std::lock_guard<std::mutex> processing_lock(state.udp_processing_mutex);
    if (!state.session.udp_receiver)
    {
        return false;
    }

    AsyncUdpDrainContext ctx{&state, &reassembler};
    const bool ok = client_async_udp_receiver_drain(state.session.udp_receiver, &ctx, handle_async_udp_packet,
                                                    WD_CLIENT_UDP_DRAIN_BATCH);
    client_reap_async_udp_receives(state);
    return ok;
}

uint64_t client_udp_next_deadline_ns(ClientState& state, const TileReassembler& reassembler, uint64_t now_ns) {
    uint64_t deadline_ns = now_ns + WD_CLIENT_MAX_IDLE_WAIT_NS;

    const uint64_t reassembly_deadline_ns = reassembler.next_expiry_deadline_ns(state);
    if (reassembly_deadline_ns != 0 && reassembly_deadline_ns < deadline_ns)
    {
        deadline_ns = reassembly_deadline_ns;
    }
    if (state.next_summary_promote_ns == 0 || state.next_summary_promote_ns <= now_ns)
    {
        deadline_ns = now_ns;
    }
    else if (state.next_summary_promote_ns < deadline_ns)
    {
        deadline_ns = state.next_summary_promote_ns;
    }

    {
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);
        if (!state.retx_queue.empty())
        {
            deadline_ns = now_ns;
        }
    }
    return deadline_ns;
}


} // namespace

ClientReceiveState* client_receive_state_create(ClientState& state) {
    auto* receive_state = new (std::nothrow) ClientReceiveState();
    if (receive_state)
    {
        receive_state->observed_config_generation = state.client_config_generation.load(std::memory_order_acquire);
    }
    return receive_state;
}

void client_receive_state_destroy(ClientReceiveState* receive_state) {
    delete receive_state;
}

bool client_receive_udp_paused(ClientState& state) {
    return client_has_pending_server_config(state);
}

bool client_receive_udp_service(ClientState& state, ClientReceiveState& receive_state) {
    const uint64_t current_generation = state.client_config_generation.load(std::memory_order_acquire);
    if (current_generation != receive_state.observed_config_generation)
    {
        receive_state.reassembler.reset();
        receive_state.observed_config_generation = current_generation;
    }

    if (client_has_pending_server_config(state))
    {
        return true;
    }

    if (!drain_udp(state, receive_state.reassembler))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> processing_lock(state.udp_processing_mutex);
        receive_state.reassembler.expire_stale_entries(state);
    }

    const uint64_t now_ns = wd_now_ns();
    if (state.next_summary_promote_ns == 0 || now_ns >= state.next_summary_promote_ns)
    {
        client_promote_deferred_summary_retransmits(state);
        state.next_summary_promote_ns = now_ns + WD_CLIENT_SUMMARY_PROMOTE_INTERVAL_NS;
    }

    if (!client_flush_retransmit_requests(state))
    {
        WD_LOG_ERROR("failed to send retransmit request");
        return false;
    }
    return true;
}

uint64_t client_receive_udp_deadline_ns(ClientState& state, const ClientReceiveState& receive_state, uint64_t now_ns) {
    if (client_has_pending_server_config(state))
    {
        return now_ns + WD_CLIENT_CONFIG_SYNC_WAIT_NS;
    }
    return client_udp_next_deadline_ns(state, receive_state.reassembler, now_ns);
}

} // namespace waydisplay
