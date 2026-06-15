#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"
#include "wd_server.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>


#define WD_ENCODER_MAX_THREADS 4u
#define WD_ENCODER_BATCH_JOBS 256u
#define WD_ENCODER_MAX_RESULTS_PER_JOB 32u

static void wd_stream_encoder_pool_destroy(struct wd_server* server);

static void wd_stream_note_input_to_first_fresh_tile_locked(struct wd_net_state* net, uint64_t tile_send_ns) {
    if (!net || !net->input_since_last_fresh_tile || net->last_input_inject_ns == 0 || tile_send_ns < net->last_input_inject_ns)
    {
        return;
    }

    net->stats.input_to_first_fresh_tile_samples++;
    net->stats.input_to_first_fresh_tile_sum_ns += tile_send_ns - net->last_input_inject_ns;
    net->input_since_last_fresh_tile = false;
}


static uint64_t wd_stream_byte_burst_cap_for_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        return 0;
    }

    uint64_t cap = bytes_per_second / WD_STREAM_TOKEN_BURST_DIVISOR;
    const uint64_t max_uncompressed_wire_tile =
        (uint64_t)WD_WIRE_TILE_MAX_WIDTH * (uint64_t)WD_WIRE_TILE_MAX_HEIGHT * (uint64_t)WD_BYTES_PER_PIXEL +
        (uint64_t)WD_UDP_TILE_HEADER_MAX_SIZE;
    if (cap < max_uncompressed_wire_tile)
    {
        cap = max_uncompressed_wire_tile;
    }
    return cap ? cap : bytes_per_second;
}

static uint64_t wd_stream_clamp_limited_udp_byte_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }

    if (bytes_per_second < WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND)
    {
        bytes_per_second = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    }

    if (bytes_per_second > WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND)
    {
        bytes_per_second = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }

    return bytes_per_second;
}

static uint64_t wd_stream_limited_rate_from_kib(uint32_t kib_per_second) {
    if (kib_per_second == 0)
    {
        return 0;
    }

    uint64_t bytes_per_second = (uint64_t)kib_per_second * 1024ull;
    if (bytes_per_second / 1024ull != (uint64_t)kib_per_second)
    {
        bytes_per_second = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }

    return wd_stream_clamp_limited_udp_byte_rate(bytes_per_second);
}

static void wd_stream_policy_reset_tokens(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    policy->last_frame_send_ns              = 0;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}

void wd_stream_policy_set_defaults(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    memset(policy, 0, sizeof(*policy));

    policy->target_fps                    = WD_DEFAULT_PARTIAL_FPS;
    policy->effective_target_fps          = WD_DEFAULT_PARTIAL_FPS;
    policy->frame_rate_good_seconds       = 0;
    policy->limited_udp_bytes_per_second  = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_floor        = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_ceiling      = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    policy->link_good_seconds             = 0;
    policy->link_loss_seconds             = 0;
    policy->multipacket_loss_cooldown_seconds = 0;
    policy->client_render_pressure_seconds = 0;
    policy->client_render_visible = true;
    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello) {
    if (!policy || !hello)
    {
        return;
    }

    uint16_t fps = hello->target_fps;
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }

    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }

    policy->target_fps           = fps;
    policy->effective_target_fps = fps;
    policy->frame_rate_good_seconds = 0;
    policy->link_good_seconds = 0;
    policy->link_loss_seconds = 0;
    policy->multipacket_loss_cooldown_seconds = 0;
    policy->client_render_pressure_seconds = 0;
    policy->client_render_visible = true;
    if (policy->limited_udp_bytes_per_second == 0)
    {
        policy->limited_udp_bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }
    if (policy->limited_udp_rate_floor == 0)
    {
        policy->limited_udp_rate_floor = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    }
    if (policy->limited_udp_rate_ceiling == 0)
    {
        policy->limited_udp_rate_ceiling = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }

    uint64_t requested_limited_rate = wd_stream_limited_rate_from_kib(hello->limited_udp_kib_per_second);
    if (requested_limited_rate != 0)
    {
        uint64_t ceiling = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_rate_ceiling);
        if (requested_limited_rate > ceiling)
        {
            requested_limited_rate = ceiling;
        }

        policy->limited_udp_bytes_per_second = requested_limited_rate;
        policy->limited_udp_rate_ceiling     = requested_limited_rate;
        if (policy->limited_udp_rate_floor > requested_limited_rate)
        {
            policy->limited_udp_rate_floor = requested_limited_rate;
        }
        policy->link_good_seconds = 0;
    }

    policy->multipacket_loss_cooldown_seconds = 0;

    wd_stream_policy_reset_tokens(policy);
}


void wd_stream_policy_set_limited_udp_byte_rate(struct wd_stream_policy* policy, uint64_t bytes_per_second) {
    if (!policy)
    {
        return;
    }

    uint64_t rate = wd_stream_clamp_limited_udp_byte_rate(bytes_per_second);

    policy->limited_udp_bytes_per_second    = rate;
    policy->limited_udp_rate_floor          = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    policy->limited_udp_rate_ceiling        = rate;
    policy->link_good_seconds       = 0;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}

static uint64_t wd_stream_policy_limited_floor(const struct wd_stream_policy* policy) {
    uint64_t floor = policy ? policy->limited_udp_rate_floor : 0;
    if (floor == 0)
    {
        floor = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
    }
    return wd_stream_clamp_limited_udp_byte_rate(floor);
}

static uint64_t wd_stream_policy_limited_ceiling(const struct wd_stream_policy* policy) {
    uint64_t ceiling = policy ? policy->limited_udp_rate_ceiling : 0;
    if (ceiling == 0)
    {
        ceiling = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
    }
    return wd_stream_clamp_limited_udp_byte_rate(ceiling);
}

static void wd_stream_policy_set_limited_rate_locked(struct wd_stream_policy* policy, uint64_t rate) {
    if (!policy)
    {
        return;
    }

    uint64_t floor   = wd_stream_policy_limited_floor(policy);
    uint64_t ceiling = wd_stream_policy_limited_ceiling(policy);

    rate = wd_stream_clamp_limited_udp_byte_rate(rate);
    if (rate < floor)
    {
        rate = floor;
    }
    if (rate > ceiling)
    {
        rate = ceiling;
    }

    if (rate == policy->limited_udp_bytes_per_second)
    {
        return;
    }

    policy->limited_udp_bytes_per_second    = rate;
    policy->limited_udp_byte_tokens         = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}


static uint16_t wd_stream_policy_effective_fps_locked(const struct wd_stream_policy* policy) {
    uint16_t fps = policy ? policy->effective_target_fps : 0;
    if (fps == 0 && policy)
    {
        fps = policy->target_fps;
    }
    if (fps == 0)
    {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }
    if (fps < WD_STREAM_FPS_MIN)
    {
        fps = WD_STREAM_FPS_MIN;
    }
    if (fps > WD_MAX_REASONABLE_FPS)
    {
        fps = WD_MAX_REASONABLE_FPS;
    }
    return fps;
}


static uint16_t wd_stream_policy_output_fps_locked(const struct wd_stream_policy* policy) {
    uint16_t fps = wd_stream_policy_effective_fps_locked(policy);

    if (policy && !policy->client_render_visible && fps > WD_STREAM_HIDDEN_CLIENT_FPS)
    {
        fps = WD_STREAM_HIDDEN_CLIENT_FPS;
    }

    if (fps < WD_STREAM_FPS_MIN)
    {
        fps = WD_STREAM_FPS_MIN;
    }

    return fps;
}


static bool wd_stream_client_packet_loss_sample(const struct wd_stats* stats) {
    if (!stats || stats->client_stats_rx == 0 || stats->client_udp_packets_rx < WD_STREAM_CLIENT_COMPLETION_MIN_PACKETS)
    {
        return false;
    }

    return stats->client_completed_packets * 100ull <
           stats->client_udp_packets_rx * (uint64_t)WD_STREAM_CLIENT_COMPLETION_LOSS_PERCENT;
}

static bool wd_stream_client_render_pressure_sample(const struct wd_stream_policy* policy, const struct wd_stats* stats) {
    if (!policy || !stats || stats->client_stats_rx == 0)
    {
        return false;
    }

    if (stats->client_render_hidden_reports != 0 || stats->client_render_visible_reports == 0)
    {
        return false;
    }

    const uint16_t effective_fps = wd_stream_policy_effective_fps_locked(policy);
    const bool client_video_activity = stats->client_tiles_completed != 0 || stats->client_udp_bytes_rx != 0 ||
                                       stats->client_present_samples != 0;

    if (effective_fps != 0 && client_video_activity &&
        stats->client_render_frames * 100ull <
            (uint64_t)effective_fps * (uint64_t)stats->client_stats_rx * WD_STREAM_CLIENT_RENDER_FPS_PRESSURE_PERCENT)
    {
        return true;
    }

    /* Present latency is useful telemetry, but a client can report high
     * SDL_RenderPresent() samples because it was allowed to over-present local
     * dirty updates, because the window system briefly blocked, or because a
     * single visible sample spiked.  Treat the client's measured render rate as
     * the render-pressure signal; if it is keeping up with the negotiated FPS,
     * present time alone should not ratchet the server down.  Input-to-present
     * latency is even broader telemetry because it includes server scheduling,
     * application damage behavior, link/repair delays, and pending input-sequence
     * correlation, so it also must not drive local client render pressure. */
    return false;
}

static bool wd_stream_client_reporting_tile_loss_locked(const struct wd_stream_policy* policy, const struct wd_stats* stats) {
    if (stats && (stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0 || wd_stream_client_packet_loss_sample(stats)))
    {
        return true;
    }

    return policy && policy->multipacket_loss_cooldown_seconds != 0;
}

static void wd_stream_policy_update_frame_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool frame_pressure,
                                                       bool strong_pressure, const char* pressure_reason) {
    if (!policy || !stats)
    {
        return;
    }

    if (policy->target_fps == 0)
    {
        policy->target_fps = WD_DEFAULT_PARTIAL_FPS;
    }
    if (policy->effective_target_fps == 0)
    {
        policy->effective_target_fps = policy->target_fps;
    }

    uint16_t old_fps = wd_stream_policy_effective_fps_locked(policy);

    if (frame_pressure)
    {
        policy->frame_rate_good_seconds = 0;

        uint32_t decrease_percent = strong_pressure ? WD_STREAM_FPS_PRESSURE_DECREASE_PERCENT : WD_STREAM_FPS_DECREASE_PERCENT;
        uint32_t new_fps = ((uint32_t)old_fps * decrease_percent) / 100u;
        if (new_fps >= old_fps && old_fps > WD_STREAM_FPS_MIN)
        {
            new_fps = old_fps - 1u;
        }
        if (new_fps < WD_STREAM_FPS_MIN)
        {
            new_fps = WD_STREAM_FPS_MIN;
        }

        if ((uint16_t)new_fps != old_fps)
        {
            policy->effective_target_fps = (uint16_t)new_fps;
            policy->last_frame_send_ns = 0;
            stats->frame_rate_downshifts++;
            WD_LOG_INFO("stream frame rate down: %u -> %u fps due to %s", old_fps, (unsigned)new_fps,
                        pressure_reason ? pressure_reason : "stream pressure");
        }
        return;
    }

    bool useful_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    if (!useful_activity)
    {
        policy->frame_rate_good_seconds = 0;
        return;
    }

    if (old_fps >= policy->target_fps)
    {
        policy->effective_target_fps = policy->target_fps;
        policy->frame_rate_good_seconds = 0;
        return;
    }

    policy->frame_rate_good_seconds++;
    if (policy->frame_rate_good_seconds < WD_STREAM_FPS_GOOD_SECONDS_TO_INCREASE)
    {
        return;
    }

    policy->frame_rate_good_seconds = 0;

    uint32_t percent_fps = ((uint32_t)old_fps * WD_STREAM_FPS_INCREASE_PERCENT) / 100u;
    uint32_t new_fps = percent_fps > (uint32_t)old_fps ? percent_fps : (uint32_t)old_fps + 1u;
    if (new_fps > policy->target_fps)
    {
        new_fps = policy->target_fps;
    }

    if ((uint16_t)new_fps != old_fps)
    {
        policy->effective_target_fps = (uint16_t)new_fps;
        policy->last_frame_send_ns = 0;
        stats->frame_rate_upshifts++;
        WD_LOG_INFO("stream frame rate up: %u -> %u fps", old_fps, (unsigned)new_fps);
    }
}

static void wd_stream_policy_update_limited_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool rate_pressure) {
    if (!policy || !stats)
    {
        return;
    }

    const bool useful_tile_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    uint64_t old_rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    uint64_t new_rate = old_rate;

    if (rate_pressure)
    {
        policy->link_good_seconds = 0;
        if (policy->link_loss_seconds < UINT32_MAX)
        {
            policy->link_loss_seconds++;
        }

        if (policy->link_loss_seconds < WD_STREAM_LINK_LOSS_SECONDS_TO_DECREASE)
        {
            return;
        }

        policy->link_loss_seconds = 0;

        new_rate = old_rate * (uint64_t)WD_STREAM_RATE_PRESSURE_DECREASE_PERCENT / 100ull;

        wd_stream_policy_set_limited_rate_locked(policy, new_rate);
        if (policy->limited_udp_bytes_per_second != old_rate)
        {
            stats->rate_decreases++;
            WD_LOG_INFO("stream byte budget down: %llu -> %llu KiB/s due to UDP send pressure",
                        (unsigned long long)(old_rate / 1024ull),
                        (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
        }
        return;
    }

    policy->link_loss_seconds = 0;
    if (!useful_tile_activity)
    {
        policy->link_good_seconds = 0;
        return;
    }

    if (policy->link_good_seconds < UINT32_MAX)
    {
        policy->link_good_seconds++;
    }
    if (policy->link_good_seconds < WD_STREAM_LINK_GOOD_SECONDS_TO_INCREASE)
    {
        return;
    }

    policy->link_good_seconds = 0;

    uint64_t percent_rate = old_rate * (uint64_t)WD_STREAM_RATE_INCREASE_PERCENT / 100ull;
    uint64_t step_rate    = old_rate + WD_STREAM_RATE_INCREASE_MIN_BYTES;
    new_rate              = percent_rate > step_rate ? percent_rate : step_rate;

    wd_stream_policy_set_limited_rate_locked(policy, new_rate);
    if (policy->limited_udp_bytes_per_second != old_rate)
    {
        stats->rate_increases++;
        WD_LOG_INFO("stream byte budget up: %llu -> %llu KiB/s",
                    (unsigned long long)(old_rate / 1024ull),
                    (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
    }
}

static void wd_stream_policy_update_health_locked(struct wd_stream_policy* policy, struct wd_stats* stats) {
    if (!policy || !stats)
    {
        return;
    }

    if (stats->client_render_hidden_reports != 0)
    {
        policy->client_render_visible = false;
    }
    else if (stats->client_render_visible_reports != 0)
    {
        policy->client_render_visible = true;
    }

    const bool send_pressure = stats->udp_send_pressure_drops != 0;
    /*
     * dirty_budget_blocked means the sender had fresh dirty work ready, but
     * the configured/probed UDP byte budget could not admit the next tile.
     * That is different from socket send pressure: the link may be healthy,
     * but the requested FPS is too high for the available stream budget and
     * frame size.  Treat it as frame-rate pressure so the sender accumulates
     * more byte tokens per output frame instead of visually dribbling partial
     * frame coverage at the nominal FPS.  Keep byte-budget adaptation tied to
     * real UDP send pressure below.
     */
    const bool budget_frame_pressure = stats->dirty_budget_blocked != 0 &&
                                       (stats->dirty_tiles != 0 || stats->udp_fresh_tiles_sent != 0);
    const bool client_render_pressure_sample = wd_stream_client_render_pressure_sample(policy, stats);
    if (!policy->client_render_visible || !client_render_pressure_sample)
    {
        policy->client_render_pressure_seconds = 0;
    }
    else if (policy->client_render_pressure_seconds < UINT32_MAX)
    {
        policy->client_render_pressure_seconds++;
    }
    const bool client_render_pressure = policy->client_render_pressure_seconds >= WD_STREAM_CLIENT_RENDER_PRESSURE_SECONDS_TO_DECREASE;
    const bool client_render_pressure_warming = client_render_pressure_sample && !client_render_pressure;
    const bool client_packet_loss = wd_stream_client_packet_loss_sample(stats);
    const bool client_tile_repair = stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0;

    if (client_tile_repair || client_packet_loss)
    {
        policy->multipacket_loss_cooldown_seconds = WD_STREAM_MULTIPACKET_LOSS_COOLDOWN_SECONDS;
    }
    else if (policy->multipacket_loss_cooldown_seconds != 0)
    {
        policy->multipacket_loss_cooldown_seconds--;
    }

    /* Tile loss is a repair signal, not a congestion signal.  It temporarily
     * disables fragmented 128x64 tiles through multipacket_loss_cooldown, but
     * it must not shrink the byte budget or frame rate by itself.  Otherwise a
     * few stale/superseded repair requests can force the sender into many more
     * tiny tiles, which creates the downward spiral seen on small-MTU links. */
    if (client_render_pressure_warming && !send_pressure && !budget_frame_pressure)
    {
        /* Client render samples are quantized to the one-second feedback cadence.
         * A capped client can occasionally report one short sample below the
         * threshold because the sample window and local present interval are not
         * phase-aligned.  Hold the current FPS while this warms up instead of
         * ratcheting down or immediately counting it as a good second. */
        policy->frame_rate_good_seconds = 0;
    }
    else
    {
        const bool stream_frame_pressure = send_pressure || budget_frame_pressure;
        const char* pressure_reason = NULL;
        if (send_pressure)
        {
            pressure_reason = "UDP send pressure";
        }
        else if (budget_frame_pressure)
        {
            pressure_reason = "UDP budget pressure";
        }
        else if (client_render_pressure)
        {
            pressure_reason = "client render pressure";
        }

        wd_stream_policy_update_frame_rate_locked(policy, stats, stream_frame_pressure || client_render_pressure,
                                                  stream_frame_pressure, pressure_reason);
    }
    wd_stream_policy_update_limited_rate_locked(policy, stats, send_pressure);
}


bool wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    bool                     client_connected  = net->client_connected;
    struct wd_stream_policy* policy            = &net->stream_policy;

    if (!client_connected || net->config_update_pending)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    if (!server->scene_dirty)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    bool should = false;

    uint16_t fps = wd_stream_policy_output_fps_locked(policy);
    uint64_t interval_ns = WD_NSEC_PER_SEC / fps;

    if (policy->last_frame_send_ns == 0 || now_ns - policy->last_frame_send_ns >= interval_ns)
    {
        policy->last_frame_send_ns = now_ns;
        should                     = true;
    }


    pthread_mutex_unlock(&net->lock);

    return should;
}

void wd_stream_invalidate_all_tiles_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (net->tiles)
    {
        for (uint16_t i = 0; i < server->total_tiles; ++i)
        {
            net->tiles[i].input_sequence = 0;
        }
    }

    if (server->damage_tiles && server->total_base_tiles > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }

    server->damage_all_tiles  = true;
    server->damage_tile_count = 0;
    server->scene_dirty       = true;
}

bool wd_stream_init(struct wd_server* server) {
    server->net.tiles = calloc(server->total_tiles, sizeof(*server->net.tiles));
    if (!server->net.tiles)
    {
        return false;
    }

    server->damage_tiles = calloc(server->total_base_tiles, sizeof(*server->damage_tiles));
    if (!server->damage_tiles)
    {
        wd_stream_destroy(server);
        return false;
    }
    server->damage_all_tiles  = true;
    server->damage_tile_count = 0;

    return true;
}

void wd_stream_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    wd_stream_encoder_pool_destroy(server);

    if (!server->net.tiles)
    {
        return;
    }

    free(server->net.tiles);
    server->net.tiles = NULL;

    free(server->damage_tiles);
    server->damage_tiles      = NULL;
    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}


static void wd_stream_free_tile_states(struct wd_tile_state* tiles, uint16_t total_tiles) {
    (void)total_tiles;
    free(tiles);
}

static void wd_stream_free_protocol_tile_state(struct wd_server* server, uint16_t tile_state_count) {
    if (!server)
    {
        return;
    }

    wd_stream_free_tile_states(server->net.tiles, tile_state_count);
    server->net.tiles = NULL;

    free(server->net.dirty_regions);
    server->net.dirty_regions = NULL;
    free(server->net.dirty_region_queued);
    server->net.dirty_region_queued = NULL;
    server->net.dirty_region_count = 0;
    free(server->net.dirty_epochs);
    server->net.dirty_epochs = NULL;
    free(server->net.dirty_queue);
    server->net.dirty_queue = NULL;
    free(server->net.dirty_queued);
    server->net.dirty_queued = NULL;
    free(server->net.dirty_queue_enqueued_ns);
    server->net.dirty_queue_enqueued_ns = NULL;
    free(server->net.retransmit_queue);
    server->net.retransmit_queue = NULL;
    free(server->net.retransmit_queued);
    server->net.retransmit_queued = NULL;
    free(server->net.retransmit_queue_enqueued_ns);
    server->net.retransmit_queue_enqueued_ns = NULL;
    free(server->net.retransmit_requested_generation);
    server->net.retransmit_requested_generation = NULL;
    free(server->net.summary_dirty_tiles);
    server->net.summary_dirty_tiles = NULL;

    free(server->net.summary_dirty_queue);
    server->net.summary_dirty_queue = NULL;
    free(server->damage_tiles);
    server->damage_tiles = NULL;
    server->damage_all_tiles = false;
    server->damage_tile_count = 0;
}

bool wd_server_reconfigure_tile_size_locked(struct wd_server* server, uint16_t tile_width, uint16_t tile_height) {
    if (!server || tile_width == 0 || tile_height == 0)
    {
        return false;
    }

    if (tile_width == server->tile_width && tile_height == server->tile_height)
    {
        return true;
    }

    const uint16_t old_total_tiles = server->total_tiles;

    if (!wd_server_set_tile_size(server, tile_width, tile_height) || !wd_server_set_geometry(server, server->display_width, server->display_height))
    {
        return false;
    }

    wd_stream_free_protocol_tile_state(server, old_total_tiles);

    server->net.dirty_regions                   = calloc(server->total_tiles, sizeof(*server->net.dirty_regions));
    server->net.dirty_region_queued             = calloc(server->total_tiles, sizeof(*server->net.dirty_region_queued));
    server->net.dirty_region_count              = 0;
    server->net.dirty_epochs                    = calloc(server->total_tiles, sizeof(*server->net.dirty_epochs));
    server->net.dirty_queue                     = calloc(server->total_tiles, sizeof(*server->net.dirty_queue));
    server->net.dirty_queued                    = calloc(server->total_tiles, sizeof(*server->net.dirty_queued));
    server->net.dirty_queue_enqueued_ns         = calloc(server->total_tiles, sizeof(*server->net.dirty_queue_enqueued_ns));
    server->net.retransmit_queue                = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue));
    server->net.retransmit_queued               = calloc(server->total_tiles, sizeof(*server->net.retransmit_queued));
    server->net.retransmit_queue_enqueued_ns    = calloc(server->total_tiles, sizeof(*server->net.retransmit_queue_enqueued_ns));
    server->net.retransmit_requested_generation = calloc(server->total_tiles, sizeof(*server->net.retransmit_requested_generation));
    server->net.summary_dirty_tiles             = calloc(server->total_tiles, sizeof(*server->net.summary_dirty_tiles));
    server->net.summary_dirty_queue             = calloc(server->total_tiles, sizeof(*server->net.summary_dirty_queue));

    if (!server->net.dirty_regions || !server->net.dirty_region_queued || !server->net.dirty_epochs || !server->net.dirty_queue ||
        !server->net.dirty_queued || !server->net.dirty_queue_enqueued_ns ||
        !server->net.retransmit_queue || !server->net.retransmit_queued || !server->net.retransmit_queue_enqueued_ns ||
        !server->net.retransmit_requested_generation || !server->net.summary_dirty_tiles || !server->net.summary_dirty_queue)
    {
        wd_stream_free_protocol_tile_state(server, server->total_tiles);
        return false;
    }

    if (!wd_stream_init(server))
    {
        wd_stream_free_protocol_tile_state(server, server->total_tiles);
        return false;
    }

    server->net.dirty_queue_read        = 0;
    server->net.dirty_queue_write       = 0;
    server->net.dirty_queue_count       = 0;
    server->net.retransmit_queue_count  = 0;
    server->net.summary_dirty_count     = 0;
    server->last_summary_ns             = 0;
    server->last_delta_summary_ns       = 0;
    server->scene_dirty                 = true;

    return true;
}

static void wd_note_udp_send_pressure_locked(struct wd_net_state* net, int send_errno) {
    if (!net)
    {
        return;
    }

    net->udp_send_pressure_drops++;
    net->stats.udp_send_pressure_drops++;

    uint64_t now = wd_now_ns();
    if (!wd_log_rate_limit_should_log(&net->udp_send_pressure_log_ns, now, WD_LOG_RATE_LIMIT_INTERVAL_NS))
    {
        return;
    }

    uint64_t drops = net->udp_send_pressure_drops;
    net->udp_send_pressure_drops = 0;

    WD_LOG_DEBUG("dropped %llu UDP tile packets under send pressure: %s", (unsigned long long)drops, strerror(send_errno));
}


static void wd_stream_mark_summary_dirty_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (!net->summary_dirty_tiles || !net->summary_dirty_queue)
    {
        return;
    }

    if (!net->summary_dirty_tiles[tile_id])
    {
        if (net->summary_dirty_count >= server->total_tiles)
        {
            return;
        }

        net->summary_dirty_tiles[tile_id] = true;
        net->summary_dirty_queue[net->summary_dirty_count++] = tile_id;
    }
}

static uint8_t wd_stream_tile_protocol_for_packet(uint16_t packet_count, uint64_t input_sequence, bool compressed_payload) {
    if (packet_count <= 1)
    {
        if (compressed_payload)
        {
            return input_sequence ? WD_TILE_COMPRESSED_SINGLE_LATENCY : WD_TILE_COMPRESSED_SINGLE;
        }

        return input_sequence ? WD_TILE_UNCOMPRESSED_SINGLE_LATENCY : WD_TILE_UNCOMPRESSED_SINGLE;
    }

    if (compressed_payload)
    {
        return input_sequence ? WD_TILE_COMPRESSED_MULTI_LATENCY : WD_TILE_COMPRESSED_MULTI;
    }

    return input_sequence ? WD_TILE_UNCOMPRESSED_MULTI_LATENCY : WD_TILE_UNCOMPRESSED_MULTI;
}

static uint32_t wd_stream_tile_wire_bytes_for_payload(uint32_t payload_size, uint16_t udp_payload_target, uint64_t input_sequence,
                                                      bool compressed_payload) {
    if (payload_size == 0)
    {
        return 0;
    }

    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        udp_payload_target = WD_UDP_TILE_PAYLOAD_MAX;
    }

    if (udp_payload_target < 512)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    uint32_t packet_count = (payload_size + udp_payload_target - 1u) / udp_payload_target;
    uint8_t protocol = wd_stream_tile_protocol_for_packet((uint16_t)packet_count, input_sequence, compressed_payload);
    uint16_t header_size = wd_udp_tile_header_size_for_protocol(protocol);
    return payload_size + packet_count * (uint32_t)header_size;
}


static bool wd_stream_use_compressed_tile_payload(uint32_t compressed_size, uint32_t uncompressed_size, uint16_t udp_payload_target,
                                                  uint64_t input_sequence) {
    if (compressed_size == 0)
    {
        return false;
    }

    uint32_t compressed_wire = wd_stream_tile_wire_bytes_for_payload(compressed_size, udp_payload_target, input_sequence, true);
    uint32_t uncompressed_wire = wd_stream_tile_wire_bytes_for_payload(uncompressed_size, udp_payload_target, input_sequence, false);

    return compressed_wire <= uncompressed_wire;
}

static void wd_stream_note_tile_choice_locked(struct wd_net_state* net, uint32_t compressed_size, uint32_t uncompressed_size,
                                              uint16_t udp_payload_target, uint64_t input_sequence, bool compressed_payload,
                                              uint16_t tile_width, uint16_t tile_height) {
    if (!net)
    {
        return;
    }

    uint32_t compressed_wire = wd_stream_tile_wire_bytes_for_payload(compressed_size, udp_payload_target, input_sequence, true);
    uint32_t uncompressed_wire = wd_stream_tile_wire_bytes_for_payload(uncompressed_size, udp_payload_target, input_sequence, false);
    uint32_t chosen_wire = compressed_payload ? compressed_wire : uncompressed_wire;
    uint32_t alternate_wire = compressed_payload ? uncompressed_wire : compressed_wire;

    if (compressed_payload)
    {
        net->stats.tile_choice_compressed++;
    }
    else
    {
        net->stats.tile_choice_uncompressed++;
    }

    if (tile_width == 128 && tile_height == 64)
    {
        net->stats.tile_size_128x64_sent++;
    }
    else if (tile_width == 64 && tile_height == 64)
    {
        net->stats.tile_size_64x64_sent++;
    }
    else if (tile_width == 32 && tile_height == 32)
    {
        net->stats.tile_size_32x32_sent++;
    }
    else if (tile_width == 16 && tile_height == 16)
    {
        net->stats.tile_size_16x16_sent++;
    }

    net->stats.tile_choice_compressed_payload_sum += compressed_size;
    net->stats.tile_choice_uncompressed_payload_sum += uncompressed_size;
    net->stats.tile_choice_compressed_wire_sum += compressed_wire;
    net->stats.tile_choice_uncompressed_wire_sum += uncompressed_wire;
    net->stats.tile_choice_chosen_wire_sum += chosen_wire;
    if (alternate_wire > chosen_wire)
    {
        net->stats.tile_choice_saved_wire_sum += alternate_wire - chosen_wire;
    }
}

static uint64_t wd_stream_policy_limited_byte_budget_locked(struct wd_stream_policy* policy, uint64_t now_ns) {
    if (!policy)
    {
        return UINT64_MAX;
    }

    uint64_t rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    policy->limited_udp_bytes_per_second = rate;

    if (policy->last_limited_udp_byte_refill_ns == 0)
    {
        policy->last_limited_udp_byte_refill_ns = now_ns;
        policy->limited_udp_byte_tokens         = 0.0;
    }
    else
    {
        uint64_t elapsed_ns = now_ns - policy->last_limited_udp_byte_refill_ns;
        double elapsed_seconds = (double)elapsed_ns / (double)WD_NSEC_PER_SEC;
        policy->limited_udp_byte_tokens += elapsed_seconds * (double)rate;

        uint64_t burst_cap = wd_stream_byte_burst_cap_for_rate(rate);
        if (policy->limited_udp_byte_tokens > (double)burst_cap)
        {
            policy->limited_udp_byte_tokens = (double)burst_cap;
        }

        policy->last_limited_udp_byte_refill_ns = now_ns;
    }

    return (uint64_t)policy->limited_udp_byte_tokens;
}

static void wd_stream_policy_consume_limited_bytes_locked(struct wd_stream_policy* policy, uint64_t bytes) {
    if (!policy || bytes == 0)
    {
        return;
    }

    if (policy->limited_udp_byte_tokens >= (double)bytes)
    {
        policy->limited_udp_byte_tokens -= (double)bytes;
    }
    else
    {
        policy->limited_udp_byte_tokens = 0.0;
    }
}

static void wd_stream_policy_refund_limited_bytes_locked(struct wd_stream_policy* policy, uint64_t bytes) {
    if (!policy || bytes == 0)
    {
        return;
    }

    uint64_t rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    policy->limited_udp_bytes_per_second = rate;

    policy->limited_udp_byte_tokens += (double)bytes;

    uint64_t burst_cap = wd_stream_byte_burst_cap_for_rate(rate);
    if (policy->limited_udp_byte_tokens > (double)burst_cap)
    {
        policy->limited_udp_byte_tokens = (double)burst_cap;
    }
}

bool wd_stream_try_consume_tcp_control_budget_locked(struct wd_net_state* net, uint32_t bytes, uint64_t now_ns) {
    if (!net || bytes == 0)
    {
        return true;
    }

    uint64_t budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now_ns);
    if (budget < (uint64_t)bytes)
    {
        net->stats.tcp_budget_blocked++;
        return false;
    }

    wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes);
    net->stats.tcp_control_bytes_sent += bytes;
    return true;
}

void wd_stream_account_tcp_control_bytes_locked(struct wd_net_state* net, uint32_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }

    wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes);
    net->stats.tcp_control_bytes_sent += bytes;
}

static void wd_stream_refund_tcp_control_budget_locked(struct wd_net_state* net, uint64_t bytes) {
    if (!net || bytes == 0)
    {
        return;
    }

    wd_stream_policy_refund_limited_bytes_locked(&net->stream_policy, bytes);
    net->stats.tcp_control_bytes_refunded += bytes;
}

static void wd_stream_init_send_result(struct wd_udp_tile_send_result* result) {
    if (!result)
    {
        return;
    }

    memset(result, 0, sizeof(*result));
}

static bool wd_stream_send_tile_payload_sized_locked(struct wd_server* server, uint16_t tile_id, uint16_t tile_width, uint16_t tile_height,
                                                     uint64_t generation, uint64_t timestamp_ns, uint64_t input_sequence,
                                                     const uint8_t* tile_payload, uint32_t tile_payload_size,
                                                     bool compressed_payload, struct wd_udp_tile_send_result* result) {
    struct wd_net_state* net = &server->net;

    wd_stream_init_send_result(result);

    const uint16_t packet_tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t packet_tiles_y = wd_tiles_for_height_with_tile(server->display_height, tile_height);
    const uint32_t packet_total_tiles = (uint32_t)packet_tiles_x * (uint32_t)packet_tiles_y;
    if (tile_width == 0 || tile_height == 0 || packet_tiles_x == 0 || packet_tiles_y == 0 || tile_id >= packet_total_tiles ||
        packet_total_tiles > UINT16_MAX)
    {
        return false;
    }

    if (!net->client_connected || tile_payload_size == 0 || !tile_payload)
    {
        return true;
    }

    uint16_t udp_payload_target = net->udp_payload_target;

    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        udp_payload_target = WD_UDP_TILE_PAYLOAD_MAX;
    }

    if (udp_payload_target < 512)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    uint16_t packet_count = (uint16_t)((tile_payload_size + udp_payload_target - 1) / udp_payload_target);
    const uint64_t udp_send_start_ns = wd_now_ns();

    for (uint16_t packet_id = 0; packet_id < packet_count; ++packet_id)
    {
        uint32_t offset = (uint32_t)packet_id * udp_payload_target;

        uint16_t payload_size =
            (uint16_t)(((tile_payload_size - offset) > udp_payload_target) ? udp_payload_target : (tile_payload_size - offset));

        uint8_t header_buf[WD_UDP_TILE_HEADER_MAX_SIZE];
        memset(header_buf, 0, sizeof(header_buf));

        uint8_t protocol = wd_stream_tile_protocol_for_packet(packet_count, input_sequence, compressed_payload);
        uint16_t header_size = wd_udp_tile_header_size_for_protocol(protocol);
        uint8_t tile_size = wd_tile_size_code_for_dimensions(tile_width, tile_height);

        switch (protocol)
        {
            case WD_TILE_UNCOMPRESSED_SINGLE: {
                struct wd_udp_tile_packet_header_uncompressed_single h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_UNCOMPRESSED_SINGLE_LATENCY: {
                struct wd_udp_tile_packet_header_uncompressed_single_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_SINGLE: {
                struct wd_udp_tile_packet_header_compressed_single h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_SINGLE_LATENCY: {
                struct wd_udp_tile_packet_header_compressed_single_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_UNCOMPRESSED_MULTI: {
                struct wd_udp_tile_packet_header_uncompressed_multi h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_UNCOMPRESSED_MULTI_LATENCY: {
                struct wd_udp_tile_packet_header_uncompressed_multi_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_MULTI: {
                struct wd_udp_tile_packet_header_compressed_multi h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            case WD_TILE_COMPRESSED_MULTI_LATENCY: {
                struct wd_udp_tile_packet_header_compressed_multi_latency h;
                memset(&h, 0, sizeof(h));
                h.session_id = net->session_id;
                h.tile_protocol = protocol;
                h.tile_flags = WD_TILE_NORMAL;
                h.tile_size = tile_size;
                h.tile_id = tile_id;
                h.tile_pkt_count = (uint8_t)packet_count;
                h.tile_pkt_id = (uint8_t)packet_id;
                h.payload_size = payload_size;
                h.tile_generation = generation;
                h.compressed_tile_size = (uint16_t)tile_payload_size;
                h.tile_timestamp_ns = timestamp_ns;
                h.input_sequence = input_sequence;
                memcpy(header_buf, &h, sizeof(h));
                break;
            }
            default:
                return false;
        }

        uint32_t packet_wire_size = (uint32_t)header_size + (uint32_t)payload_size;
        bool async_queued = wd_async_udp_send_packet(net->udp_tx, net->udp_fd, &net->client_udp_addr, header_buf,
                                                     header_size, (uint8_t*)tile_payload + offset, payload_size);
        if (!async_queued)
        {
            if (net->udp_tx)
            {
                net->stats.udp_async_send_failed++;
            }

            struct iovec iov[2];
            memset(iov, 0, sizeof(iov));
            iov[0].iov_base = header_buf;
            iov[0].iov_len  = header_size;
            iov[1].iov_base = (uint8_t*)tile_payload + offset;
            iov[1].iov_len  = payload_size;

            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_name    = &net->client_udp_addr;
            msg.msg_namelen = sizeof(net->client_udp_addr);
            msg.msg_iov     = iov;
            msg.msg_iovlen  = 2;

            ssize_t sent = sendmsg(net->udp_fd, &msg, 0);

            if (sent < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
                {
                    wd_note_udp_send_pressure_locked(net, errno);
                    if (result)
                    {
                        result->send_blocked = true;
                    }
                    break;
                }

                WD_LOG_ERROR("sendto failed: %s", strerror(errno));
                return false;
            }

            packet_wire_size = (uint32_t)sent;
        }

        if (result)
        {
            result->any_packet_sent = true;
            result->packets_sent++;
            result->bytes_sent += packet_wire_size;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)packet_wire_size;
    }

    if (net->udp_tx && !wd_async_udp_sender_flush(net->udp_tx))
    {
        net->stats.udp_async_send_failed++;
    }

    if (result)
    {
        result->all_packets_sent = result->packets_sent == packet_count;
        if (result->any_packet_sent && !result->all_packets_sent)
        {
            net->stats.partial_tile_sends++;
            net->stats.partial_tile_packets_sent += result->packets_sent;
        }
    }

    if (!result || result->all_packets_sent)
    {
        net->stats.udp_tiles_sent++;
        if (compressed_payload)
        {
            net->stats.udp_compressed_tiles_sent++;
            net->stats.udp_compressed_tile_bytes_sent += tile_payload_size;
        }
        else
        {
            net->stats.udp_uncompressed_tiles_sent++;
            net->stats.udp_uncompressed_tile_bytes_sent += tile_payload_size;
        }
        if (tile_width == server->tile_width && tile_height == server->tile_height)
        {
            wd_stream_mark_summary_dirty_locked(server, tile_id);
        }
    }

    net->stats.udp_send_ns += wd_now_ns() - udp_send_start_ns;
    return true;
}

static void wd_dirty_queue_note_cleared_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || !net->dirty_queued || tile_id >= total_tiles || !net->dirty_queued[tile_id])
    {
        return;
    }

    net->dirty_queued[tile_id] = false;
    if (net->dirty_queue_count > 0)
    {
        net->dirty_queue_count--;
    }
    if (net->dirty_queue_enqueued_ns)
    {
        net->dirty_queue_enqueued_ns[tile_id] = 0;
    }
}

static void wd_stream_note_queue_age_locked(uint64_t enqueued_ns, uint64_t* samples, uint64_t* sum_ns) {
    if (!enqueued_ns || !samples || !sum_ns)
    {
        return;
    }

    uint64_t now = wd_now_ns();
    if (now < enqueued_ns)
    {
        return;
    }

    (*samples)++;
    *sum_ns += now - enqueued_ns;
}

static bool wd_dirty_queue_push_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || tile_id >= total_tiles || !net->dirty_queued)
    {
        return false;
    }

    if (net->dirty_queued[tile_id])
    {
        return true;
    }

    if (net->dirty_queue_count >= total_tiles)
    {
        return false;
    }

    /* Fresh content supersedes any queued repair for the same base tile. */
    if (net->retransmit_queued && net->retransmit_queued[tile_id])
    {
        net->retransmit_queued[tile_id] = false;
        if (net->retransmit_queue_enqueued_ns)
        {
            net->retransmit_queue_enqueued_ns[tile_id] = 0;
        }
        if (net->retransmit_requested_generation)
        {
            net->retransmit_requested_generation[tile_id] = 0;
        }
        net->stats.retx_tiles_superseded_by_fresh++;
    }

    net->dirty_queued[tile_id] = true;
    net->dirty_queue_count++;
    if (net->dirty_queue_enqueued_ns)
    {
        net->dirty_queue_enqueued_ns[tile_id] = wd_now_ns();
    }

    return true;
}

bool wd_stream_queue_retransmit_tile_locked(struct wd_server* server, uint16_t tile_id, uint64_t requested_generation) {
    if (!server || tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    if (!net->retransmit_queue || !net->retransmit_queued)
    {
        return false;
    }

    if (net->dirty_queued && net->dirty_queued[tile_id])
    {
        net->stats.retx_tiles_superseded_by_fresh++;
        return false;
    }

    if (net->retransmit_queued[tile_id])
    {
        if (net->retransmit_requested_generation && net->retransmit_requested_generation[tile_id] < requested_generation)
        {
            net->retransmit_requested_generation[tile_id] = requested_generation;
        }
        return false;
    }

    if (net->retransmit_queue_count >= server->total_tiles)
    {
        return false;
    }

    net->retransmit_queue[net->retransmit_queue_count++] = tile_id;
    net->retransmit_queued[tile_id] = true;
    if (net->retransmit_requested_generation)
    {
        net->retransmit_requested_generation[tile_id] = requested_generation;
    }
    if (net->retransmit_queue_enqueued_ns)
    {
        net->retransmit_queue_enqueued_ns[tile_id] = wd_now_ns();
    }
    server->scene_dirty = true;

    return true;
}

static void wd_clear_damage_tiles(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    if (server->damage_tiles && server->damage_tile_count > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_base_tiles * sizeof(*server->damage_tiles));
    }

    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}

static void wd_stream_mark_dirty_top_region_locked(struct wd_server* server, uint16_t base_tile_id);
static void wd_stream_maybe_clear_dirty_top_region_for_base_locked(struct wd_server* server, uint16_t base_tile_id);

static void wd_detect_one_dirty_tile_into_queue_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (net->dirty_epochs)
    {
        net->dirty_epochs[tile_id]++;
        if (net->dirty_epochs[tile_id] == 0)
        {
            net->dirty_epochs[tile_id] = 1;
        }
    }
    if (wd_dirty_queue_push_locked(net, tile_id, server->total_tiles))
    {
        wd_stream_mark_dirty_top_region_locked(server, tile_id);
    }
    else if (net->dirty_queued && net->dirty_queued[tile_id])
    {
        wd_stream_mark_dirty_top_region_locked(server, tile_id);
    }
}

static void wd_detect_dirty_tiles_into_queue_locked(struct wd_server* server) {
    if (!server || !server->net.tiles)
    {
        return;
    }

    if (server->damage_all_tiles || !server->damage_tiles)
    {
        for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
        {
            wd_detect_one_dirty_tile_into_queue_locked(server, tile_id);
        }

        wd_clear_damage_tiles(server);
        return;
    }

    if (server->damage_tile_count > 0)
    {
        const uint32_t limit = server->total_base_tiles < server->total_tiles ? server->total_base_tiles : server->total_tiles;
        for (uint32_t tile_id = 0; tile_id < limit; ++tile_id)
        {
            if (server->damage_tiles[tile_id])
            {
                wd_detect_one_dirty_tile_into_queue_locked(server, (uint16_t)tile_id);
            }
        }
    }

    wd_clear_damage_tiles(server);
}


struct wd_wire_tile_candidate {
    uint16_t width;
    uint16_t height;
    uint16_t tile_id;
    uint16_t covered_base_ids[64];
    uint16_t covered_base_count;
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint32_t wire_size;
    bool compressed_payload;
};


struct wd_parallel_encode_result {
    bool valid;
    bool budget_blocked;
    uint16_t top_region_id;
    struct wd_wire_tile_candidate candidate;
    uint64_t covered_dirty_epochs[64];
    uint8_t* payload;
    uint32_t payload_size;
    uint64_t worker_encode_ns;
    uint64_t framebuffer_generation;
};

struct wd_parallel_encode_job {
    struct wd_server* server;
    const uint32_t* framebuffer_xrgb8888;
    uint32_t display_width;
    uint32_t display_height;
    uint16_t tile_width;
    uint16_t tile_height;
    uint16_t tiles_x;
    uint16_t tiles_y;
    uint16_t total_tiles;
    uint16_t top_region_id;
    uint64_t input_sequence;
    uint64_t remaining_byte_budget;
    uint64_t framebuffer_generation;
    uint16_t udp_payload_target;
    bool network_happy;
    const bool* dirty_snapshot;
    const uint64_t* dirty_epoch_snapshot;
    struct wd_parallel_encode_result* result;
    uint16_t result_capacity;
    uint16_t result_count;
};

struct wd_encoder_worker_state {
    struct wd_encoder_pool* pool;
    uint16_t worker_index;
    pthread_t thread;
    uint8_t* tile_bytes;
    uint8_t* compressed_tile;
    size_t compressed_capacity;
};

struct wd_parallel_encode_batch {
    struct wd_parallel_encode_job* jobs;
    uint16_t job_count;
    uint16_t next_job;
    uint16_t completed_jobs;
    uint64_t worker_encode_ns;
    bool active;
};

struct wd_encoder_pool {
    pthread_mutex_t lock;
    pthread_cond_t work_cond;
    pthread_cond_t done_cond;
    bool running;
    uint16_t thread_count;
    struct wd_parallel_encode_batch* batch;
    struct wd_encoder_worker_state workers[WD_ENCODER_MAX_THREADS];
};

static void* wd_stream_encoder_worker_main(void* data);

static uint16_t wd_stream_encoder_thread_count(void) {
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 2)
    {
        return 1;
    }
    if (cpu_count > (long)WD_ENCODER_MAX_THREADS + 1)
    {
        return WD_ENCODER_MAX_THREADS;
    }
    return (uint16_t)(cpu_count - 1);
}


static void wd_stream_encoder_pool_destroy(struct wd_server* server) {
    if (!server || !server->net.encoder_pool)
    {
        return;
    }

    struct wd_encoder_pool* pool = server->net.encoder_pool;
    pthread_mutex_lock(&pool->lock);
    pool->running = false;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->lock);

    for (uint16_t i = 0; i < pool->thread_count; ++i)
    {
        pthread_join(pool->workers[i].thread, NULL);
        free(pool->workers[i].tile_bytes);
        pool->workers[i].tile_bytes = NULL;
        free(pool->workers[i].compressed_tile);
        pool->workers[i].compressed_tile = NULL;
    }

    pthread_cond_destroy(&pool->done_cond);
    pthread_cond_destroy(&pool->work_cond);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
    server->net.encoder_pool = NULL;
}

static bool wd_stream_encoder_pool_ensure(struct wd_server* server) {
    if (!server)
    {
        return false;
    }
    if (server->net.encoder_pool)
    {
        return true;
    }

    struct wd_encoder_pool* pool = calloc(1, sizeof(*pool));
    if (!pool)
    {
        return false;
    }
    if (pthread_mutex_init(&pool->lock, NULL) != 0)
    {
        free(pool);
        return false;
    }
    if (pthread_cond_init(&pool->work_cond, NULL) != 0)
    {
        pthread_mutex_destroy(&pool->lock);
        free(pool);
        return false;
    }
    if (pthread_cond_init(&pool->done_cond, NULL) != 0)
    {
        pthread_cond_destroy(&pool->work_cond);
        pthread_mutex_destroy(&pool->lock);
        free(pool);
        return false;
    }

    const uint32_t max_wire_tile_bytes = WD_WIRE_TILE_MAX_WIDTH * WD_WIRE_TILE_MAX_HEIGHT * WD_BYTES_PER_PIXEL;
    const size_t compressed_capacity = wd_zstd_compress_bound(max_wire_tile_bytes);
    pool->thread_count = wd_stream_encoder_thread_count();
    if (pool->thread_count == 0)
    {
        pool->thread_count = 1;
    }
    pool->running = true;
    server->net.encoder_pool = pool;

    for (uint16_t i = 0; i < pool->thread_count; ++i)
    {
        pool->workers[i].pool = pool;
        pool->workers[i].worker_index = i;
        pool->workers[i].compressed_capacity = compressed_capacity;
        pool->workers[i].tile_bytes = malloc(max_wire_tile_bytes);
        pool->workers[i].compressed_tile = malloc(compressed_capacity);
        if (!pool->workers[i].tile_bytes || !pool->workers[i].compressed_tile ||
            pthread_create(&pool->workers[i].thread, NULL, wd_stream_encoder_worker_main, &pool->workers[i]) != 0)
        {
            free(pool->workers[i].tile_bytes);
            pool->workers[i].tile_bytes = NULL;
            free(pool->workers[i].compressed_tile);
            pool->workers[i].compressed_tile = NULL;
            pool->thread_count = i;
            wd_stream_encoder_pool_destroy(server);
            return false;
        }
    }

    return true;
}

static bool wd_stream_encoder_pool_run(struct wd_server* server, struct wd_parallel_encode_batch* batch,
                                       uint16_t* out_worker_threads) {
    if (out_worker_threads)
    {
        *out_worker_threads = 0;
    }
    if (!server || !batch || batch->job_count == 0 || !wd_stream_encoder_pool_ensure(server))
    {
        return false;
    }

    struct wd_encoder_pool* pool = server->net.encoder_pool;
    pthread_mutex_lock(&pool->lock);
    batch->next_job = 0;
    batch->completed_jobs = 0;
    batch->worker_encode_ns = 0;
    batch->active = true;
    pool->batch = batch;
    pthread_cond_broadcast(&pool->work_cond);
    while (batch->active)
    {
        pthread_cond_wait(&pool->done_cond, &pool->lock);
    }
    pool->batch = NULL;
    if (out_worker_threads)
    {
        *out_worker_threads = pool->thread_count;
    }
    pthread_mutex_unlock(&pool->lock);
    return true;
}

void wd_stream_wait_for_encoder_idle_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }
    while (server->net.encoder_batch_active)
    {
        pthread_cond_wait(&server->net.encoder_idle_cond, &server->net.lock);
    }
}

static bool wd_stream_collect_wire_tile_base_ids(const struct wd_server* server, uint16_t tile_id, uint16_t tile_width,
                                                 uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count,
                                                 uint16_t max_count) {
    if (!server || !out_ids || !out_count || tile_width == 0 || tile_height == 0 || server->tile_width == 0 || server->tile_height == 0)
    {
        return false;
    }

    *out_count = 0;
    const uint16_t tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    if (tiles_x == 0)
    {
        return false;
    }

    const uint32_t x = wd_tile_start_x_for_tile(tile_id, tiles_x, tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(tile_id, tiles_x, tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(server->display_width, tile_id, tiles_x, tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(server->display_height, tile_id, tiles_x, tile_height);
    if (w == 0 || h == 0)
    {
        return false;
    }

    uint32_t bx0 = x / server->tile_width;
    uint32_t by0 = y / server->tile_height;
    uint32_t bx1 = (x + w - 1u) / server->tile_width;
    uint32_t by1 = (y + h - 1u) / server->tile_height;
    if (bx1 >= server->tiles_x)
    {
        bx1 = (uint32_t)server->tiles_x - 1u;
    }
    if (by1 >= server->tiles_y)
    {
        by1 = (uint32_t)server->tiles_y - 1u;
    }

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            uint32_t base_id = by * (uint32_t)server->tiles_x + bx;
            if (base_id >= server->total_tiles || *out_count >= max_count)
            {
                return false;
            }
            out_ids[(*out_count)++] = (uint16_t)base_id;
        }
    }
    return *out_count != 0;
}

static bool wd_stream_wire_tile_for_pixel(const struct wd_server* server, uint32_t x, uint32_t y, uint16_t tile_width,
                                          uint16_t tile_height, uint16_t* out_tile_id) {
    if (!server || !out_tile_id || tile_width == 0 || tile_height == 0 || x >= server->display_width || y >= server->display_height)
    {
        return false;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t tiles_y = wd_tiles_for_height_with_tile(server->display_height, tile_height);
    if (tiles_x == 0 || tiles_y == 0)
    {
        return false;
    }

    const uint32_t tx = x / tile_width;
    const uint32_t ty = y / tile_height;
    const uint32_t id = ty * (uint32_t)tiles_x + tx;
    if (tx >= tiles_x || ty >= tiles_y || id > UINT16_MAX)
    {
        return false;
    }

    *out_tile_id = (uint16_t)id;
    return true;
}


static bool wd_stream_top_region_for_base_tile(const struct wd_server* server, uint16_t base_tile_id, uint16_t* out_region_id) {
    if (!server || !out_region_id || base_tile_id >= server->total_tiles || server->tiles_x == 0)
    {
        return false;
    }

    const uint32_t bx = (uint32_t)base_tile_id % server->tiles_x;
    const uint32_t by = (uint32_t)base_tile_id / server->tiles_x;
    const uint32_t x = bx * server->tile_width;
    const uint32_t y = by * server->tile_height;
    return wd_stream_wire_tile_for_pixel(server, x, y, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, out_region_id);
}

static void wd_stream_mark_dirty_top_region_locked(struct wd_server* server, uint16_t base_tile_id) {
    if (!server || !server->net.dirty_regions || !server->net.dirty_region_queued)
    {
        return;
    }

    uint16_t region_id = 0;
    if (!wd_stream_top_region_for_base_tile(server, base_tile_id, &region_id))
    {
        return;
    }

    const uint16_t top_total = wd_total_tiles_for_size_with_tile(server->display_width, server->display_height,
                                                                 WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT);
    if (region_id >= top_total || server->net.dirty_region_queued[region_id])
    {
        return;
    }

    server->net.dirty_region_queued[region_id] = true;
    server->net.dirty_regions[server->net.dirty_region_count++] = region_id;
}

static void wd_stream_remove_dirty_top_region_locked(struct wd_server* server, uint16_t region_id) {
    if (!server || !server->net.dirty_regions || !server->net.dirty_region_queued ||
        !server->net.dirty_region_queued[region_id])
    {
        return;
    }

    for (uint16_t i = 0; i < server->net.dirty_region_count; ++i)
    {
        if (server->net.dirty_regions[i] == region_id)
        {
            server->net.dirty_regions[i] = server->net.dirty_regions[server->net.dirty_region_count - 1];
            server->net.dirty_region_count--;
            break;
        }
    }
    server->net.dirty_region_queued[region_id] = false;
}

static bool wd_stream_top_region_still_dirty_locked(struct wd_server* server, uint16_t region_id) {
    if (!server || !server->net.dirty_queued)
    {
        return false;
    }

    uint16_t ids[64];
    uint16_t count = 0;
    if (!wd_stream_collect_wire_tile_base_ids(server, region_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))))
    {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i)
    {
        if (ids[i] < server->total_tiles && server->net.dirty_queued[ids[i]])
        {
            return true;
        }
    }
    return false;
}

static void wd_stream_maybe_clear_dirty_top_region_for_base_locked(struct wd_server* server, uint16_t base_tile_id) {
    uint16_t region_id = 0;
    if (!wd_stream_top_region_for_base_tile(server, base_tile_id, &region_id))
    {
        return;
    }
    if (!wd_stream_top_region_still_dirty_locked(server, region_id))
    {
        wd_stream_remove_dirty_top_region_locked(server, region_id);
    }
}

static bool wd_stream_candidate_allowed_for_region_locked(struct wd_server* server, const struct wd_wire_tile_candidate* candidate,
                                                          uint64_t remaining_byte_budget, bool network_happy) {
    if (!server || !candidate || candidate->wire_size == 0 || candidate->wire_size > remaining_byte_budget)
    {
        return false;
    }

    const uint32_t one_packet_budget = (uint32_t)server->net.udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
    const bool is_max_tile = candidate->width == WD_WIRE_TILE_MAX_WIDTH && candidate->height == WD_WIRE_TILE_MAX_HEIGHT;
    if (candidate->width == server->tile_width && candidate->height == server->tile_height)
    {
        return true;
    }
    if (is_max_tile && network_happy)
    {
        return true;
    }
    return candidate->wire_size <= one_packet_budget;
}

static bool wd_stream_candidate_allowed_for_job(const struct wd_parallel_encode_job* job,
                                                const struct wd_wire_tile_candidate* candidate) {
    if (!job || !candidate || candidate->wire_size == 0 || candidate->wire_size > job->remaining_byte_budget)
    {
        return false;
    }

    const uint32_t one_packet_budget = (uint32_t)job->udp_payload_target + WD_UDP_TILE_HEADER_MAX_SIZE;
    const bool is_max_tile = candidate->width == WD_WIRE_TILE_MAX_WIDTH && candidate->height == WD_WIRE_TILE_MAX_HEIGHT;
    if (candidate->width == job->tile_width && candidate->height == job->tile_height)
    {
        return true;
    }
    if (is_max_tile && job->network_happy)
    {
        return true;
    }
    return candidate->wire_size <= one_packet_budget;
}

static bool wd_stream_job_collect_wire_tile_base_ids(const struct wd_parallel_encode_job* job, uint16_t tile_id,
                                                        uint16_t tile_width, uint16_t tile_height, uint16_t* out_ids,
                                                        uint16_t* out_count, uint16_t max_count) {
    if (!job || !out_ids || !out_count || tile_width == 0 || tile_height == 0 || job->tile_width == 0 ||
        job->tile_height == 0)
    {
        return false;
    }

    *out_count = 0;
    const uint16_t tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    if (tiles_x == 0)
    {
        return false;
    }

    const uint32_t x = wd_tile_start_x_for_tile(tile_id, tiles_x, tile_width);
    const uint32_t y = wd_tile_start_y_for_tile(tile_id, tiles_x, tile_height);
    const uint32_t w = wd_tile_visible_width_for_tile(job->display_width, tile_id, tiles_x, tile_width);
    const uint32_t h = wd_tile_visible_height_for_tile(job->display_height, tile_id, tiles_x, tile_height);
    if (w == 0 || h == 0)
    {
        return false;
    }

    uint32_t bx0 = x / job->tile_width;
    uint32_t by0 = y / job->tile_height;
    uint32_t bx1 = (x + w - 1u) / job->tile_width;
    uint32_t by1 = (y + h - 1u) / job->tile_height;
    if (bx1 >= job->tiles_x)
    {
        bx1 = (uint32_t)job->tiles_x - 1u;
    }
    if (by1 >= job->tiles_y)
    {
        by1 = (uint32_t)job->tiles_y - 1u;
    }

    for (uint32_t by = by0; by <= by1; ++by)
    {
        for (uint32_t bx = bx0; bx <= bx1; ++bx)
        {
            uint32_t base_id = by * (uint32_t)job->tiles_x + bx;
            if (base_id >= job->total_tiles || *out_count >= max_count)
            {
                return false;
            }
            out_ids[(*out_count)++] = (uint16_t)base_id;
        }
    }
    return *out_count != 0;
}

static bool wd_stream_job_wire_tile_for_pixel(const struct wd_parallel_encode_job* job, uint32_t x, uint32_t y,
                                              uint16_t tile_width, uint16_t tile_height, uint16_t* out_tile_id) {
    if (!job || !out_tile_id || tile_width == 0 || tile_height == 0 || x >= job->display_width || y >= job->display_height)
    {
        return false;
    }

    const uint16_t tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint16_t tiles_y = wd_tiles_for_height_with_tile(job->display_height, tile_height);
    if (tiles_x == 0 || tiles_y == 0)
    {
        return false;
    }

    const uint32_t tx = x / tile_width;
    const uint32_t ty = y / tile_height;
    const uint32_t id = ty * (uint32_t)tiles_x + tx;
    if (tx >= tiles_x || ty >= tiles_y || id > UINT16_MAX)
    {
        return false;
    }

    *out_tile_id = (uint16_t)id;
    return true;
}

static bool wd_stream_snapshot_region_has_dirty(const struct wd_parallel_encode_job* job, const bool* dirty_snapshot,
                                                uint16_t wire_tile_id, uint16_t tile_width, uint16_t tile_height,
                                                uint16_t* out_ids, uint16_t* out_count, uint16_t max_count) {
    if (!job || !dirty_snapshot || !out_ids || !out_count)
    {
        return false;
    }
    if (!wd_stream_job_collect_wire_tile_base_ids(job, wire_tile_id, tile_width, tile_height, out_ids, out_count, max_count))
    {
        return false;
    }
    for (uint16_t i = 0; i < *out_count; ++i)
    {
        const uint16_t base_id = out_ids[i];
        if (base_id < job->total_tiles && dirty_snapshot[base_id])
        {
            return true;
        }
    }
    return false;
}

static bool wd_stream_try_encode_candidate_for_snapshot(const struct wd_parallel_encode_job* job, uint16_t wire_tile_id,
                                                        uint16_t tile_width, uint16_t tile_height, bool allow_compression,
                                                        uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                        size_t compressed_capacity, struct wd_wire_tile_candidate* out,
                                                        uint64_t* out_epochs) {
    if (!job || !job->server || !job->framebuffer_xrgb8888 || !tile_bytes || !compressed_tile || !out || !out_epochs)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_job_collect_wire_tile_base_ids(job, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                             (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const uint32_t uncompressed_size = (uint32_t)tile_width * (uint32_t)tile_height * WD_BYTES_PER_PIXEL;
    const uint16_t wire_tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint16_t wire_total_tiles = wd_total_tiles_for_size_with_tile(job->display_width, job->display_height, tile_width, tile_height);
    if (!wd_extract_tile_xrgb8888_for_tile(job->framebuffer_xrgb8888, job->display_width, job->display_height,
                                           wire_tiles_x, wire_total_tiles, wire_tile_id, tile_width, tile_height, tile_bytes))
    {
        return false;
    }

    uint32_t compressed_size = 0;
    bool compressed_payload = false;
    if (allow_compression)
    {
        if (!wd_zstd_compress(tile_bytes, uncompressed_size, compressed_tile, (uint32_t)compressed_capacity, WD_ZSTD_LEVEL,
                              &compressed_size))
        {
            return false;
        }
        compressed_payload = wd_stream_use_compressed_tile_payload(compressed_size, uncompressed_size,
                                                                   job->udp_payload_target, job->input_sequence);
    }

    const uint32_t payload_size = compressed_payload ? compressed_size : uncompressed_size;
    memset(out, 0, sizeof(*out));
    out->width = tile_width;
    out->height = tile_height;
    out->tile_id = wire_tile_id;
    memcpy(out->covered_base_ids, covered_ids, (size_t)covered_count * sizeof(covered_ids[0]));
    out->covered_base_count = covered_count;
    out->uncompressed_size = uncompressed_size;
    out->compressed_size = compressed_size;
    out->wire_size = wd_stream_tile_wire_bytes_for_payload(payload_size, job->udp_payload_target,
                                                           job->input_sequence, compressed_payload);
    out->compressed_payload = compressed_payload;
    for (uint16_t i = 0; i < covered_count; ++i)
    {
        out_epochs[i] = job->dirty_epoch_snapshot ? job->dirty_epoch_snapshot[covered_ids[i]] : 0;
    }
    return true;
}

static bool wd_stream_append_snapshot_result(struct wd_parallel_encode_job* job,
                                             const struct wd_wire_tile_candidate* candidate,
                                             const uint64_t* covered_epochs,
                                             const uint8_t* tile_bytes,
                                             const uint8_t* compressed_tile) {
    if (!job || !candidate || !covered_epochs || !tile_bytes || !compressed_tile || !job->result ||
        job->result_count >= job->result_capacity)
    {
        return false;
    }

    const uint8_t* payload = candidate->compressed_payload ? compressed_tile : tile_bytes;
    const uint32_t payload_size = candidate->compressed_payload ? candidate->compressed_size : candidate->uncompressed_size;
    struct wd_parallel_encode_result* result = &job->result[job->result_count];
    memset(result, 0, sizeof(*result));
    result->top_region_id = job->top_region_id;
    result->framebuffer_generation = job->framebuffer_generation;
    result->payload = malloc(payload_size);
    if (!result->payload)
    {
        return false;
    }

    memcpy(result->payload, payload, payload_size);
    result->payload_size = payload_size;
    result->candidate = *candidate;
    memcpy(result->covered_dirty_epochs, covered_epochs,
           (size_t)candidate->covered_base_count * sizeof(covered_epochs[0]));
    result->valid = true;
    job->result_count++;
    return true;
}

static bool wd_stream_encode_region_recursive_snapshot(struct wd_parallel_encode_job* job, uint16_t wire_tile_id,
                                                       uint16_t tile_width, uint16_t tile_height,
                                                       uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                       size_t compressed_capacity, bool* out_budget_blocked) {
    if (out_budget_blocked)
    {
        *out_budget_blocked = false;
    }
    if (!job || !job->server || !tile_bytes || !compressed_tile)
    {
        return false;
    }
    if (job->result_count >= job->result_capacity)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_snapshot_region_has_dirty(job, job->dirty_snapshot, wire_tile_id, tile_width, tile_height,
                                             covered_ids, &covered_count, (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const bool is_base_tile = tile_width == job->tile_width && tile_height == job->tile_height;
    const bool allow_compression = !is_base_tile;
    struct wd_wire_tile_candidate candidate;
    uint64_t candidate_epochs[64] = {0};
    memset(&candidate, 0, sizeof(candidate));
    if (wd_stream_try_encode_candidate_for_snapshot(job, wire_tile_id, tile_width, tile_height, allow_compression,
                                                    tile_bytes, compressed_tile, compressed_capacity, &candidate,
                                                    candidate_epochs) &&
        wd_stream_candidate_allowed_for_job(job, &candidate))
    {
        return wd_stream_append_snapshot_result(job, &candidate, candidate_epochs, tile_bytes, compressed_tile);
    }

    if (is_base_tile)
    {
        if (candidate.wire_size > job->remaining_byte_budget && out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
        return false;
    }

    const uint16_t parent_tiles_x = wd_tiles_for_width_with_tile(job->display_width, tile_width);
    const uint32_t start_x = wd_tile_start_x_for_tile(wire_tile_id, parent_tiles_x, tile_width);
    const uint32_t start_y = wd_tile_start_y_for_tile(wire_tile_id, parent_tiles_x, tile_height);
    uint16_t child_width = 0;
    uint16_t child_height = 0;
    uint16_t child_count = 0;
    uint16_t child_ids[4];

    if (tile_width == WD_WIRE_TILE_MAX_WIDTH && tile_height == WD_WIRE_TILE_MAX_HEIGHT)
    {
        child_width = 64;
        child_height = 64;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        for (uint16_t i = 0; i < 2; ++i)
        {
            if (xs[i] < job->display_width && start_y < job->display_height &&
                wd_stream_job_wire_tile_for_pixel(job, xs[i], start_y, child_width, child_height, &child_ids[child_count]))
            {
                child_count++;
            }
        }
    }
    else if (tile_width == 64 && tile_height == 64)
    {
        child_width = 32;
        child_height = 32;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        const uint32_t ys[2] = {start_y, start_y + child_height};
        for (uint16_t y = 0; y < 2; ++y)
        {
            for (uint16_t x = 0; x < 2; ++x)
            {
                if (xs[x] < job->display_width && ys[y] < job->display_height &&
                    wd_stream_job_wire_tile_for_pixel(job, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
                {
                    child_count++;
                }
            }
        }
    }
    else if (tile_width == 32 && tile_height == 32)
    {
        child_width = job->tile_width;
        child_height = job->tile_height;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        const uint32_t ys[2] = {start_y, start_y + child_height};
        for (uint16_t y = 0; y < 2; ++y)
        {
            for (uint16_t x = 0; x < 2; ++x)
            {
                if (xs[x] < job->display_width && ys[y] < job->display_height &&
                    wd_stream_job_wire_tile_for_pixel(job, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
                {
                    child_count++;
                }
            }
        }
    }
    else
    {
        return false;
    }

    bool any_encoded = false;
    bool any_budget_blocked = false;
    for (uint16_t i = 0; i < child_count; ++i)
    {
        bool child_budget_blocked = false;
        if (wd_stream_encode_region_recursive_snapshot(job, child_ids[i], child_width, child_height, tile_bytes,
                                                       compressed_tile, compressed_capacity, &child_budget_blocked))
        {
            any_encoded = true;
        }
        if (child_budget_blocked)
        {
            any_budget_blocked = true;
        }
        if (job->result_count >= job->result_capacity)
        {
            break;
        }
    }
    if (any_budget_blocked && out_budget_blocked)
    {
        *out_budget_blocked = true;
    }
    return any_encoded;
}

static void wd_stream_parallel_encode_one_job(struct wd_parallel_encode_job* job, uint8_t* tile_bytes,
                                              uint8_t* compressed_tile, size_t compressed_capacity) {
    if (!job || !job->result || !job->server || !tile_bytes || !compressed_tile)
    {
        return;
    }
    for (uint16_t i = 0; i < job->result_capacity; ++i)
    {
        memset(&job->result[i], 0, sizeof(job->result[i]));
        job->result[i].top_region_id = job->top_region_id;
        job->result[i].framebuffer_generation = job->framebuffer_generation;
    }
    job->result_count = 0;

    const uint64_t start_ns = wd_now_ns();
    bool budget_blocked = false;
    if (!wd_stream_encode_region_recursive_snapshot(job, job->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                    WD_WIRE_TILE_MAX_HEIGHT, tile_bytes, compressed_tile,
                                                    compressed_capacity, &budget_blocked))
    {
        job->result[0].budget_blocked = budget_blocked;
    }
    for (uint16_t i = 0; i < job->result_count; ++i)
    {
        job->result[i].worker_encode_ns = 0;
    }
    job->result[0].worker_encode_ns = wd_now_ns() - start_ns;
}

static void* wd_stream_encoder_worker_main(void* data) {
    struct wd_encoder_worker_state* worker = data;
    if (!worker || !worker->pool)
    {
        return NULL;
    }

    struct wd_encoder_pool* pool = worker->pool;
    for (;;)
    {
        pthread_mutex_lock(&pool->lock);
        while (pool->running && (!pool->batch || !pool->batch->active || pool->batch->next_job >= pool->batch->job_count))
        {
            pthread_cond_wait(&pool->work_cond, &pool->lock);
        }

        if (!pool->running)
        {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        struct wd_parallel_encode_batch* batch = pool->batch;
        uint16_t index = batch->next_job++;
        pthread_mutex_unlock(&pool->lock);

        wd_stream_parallel_encode_one_job(&batch->jobs[index], worker->tile_bytes, worker->compressed_tile,
                                          worker->compressed_capacity);

        pthread_mutex_lock(&pool->lock);
        batch->worker_encode_ns += batch->jobs[index].result ? batch->jobs[index].result->worker_encode_ns : 0;
        batch->completed_jobs++;
        if (batch->completed_jobs >= batch->job_count)
        {
            batch->active = false;
            pthread_cond_signal(&pool->done_cond);
        }
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

static void wd_stream_compact_retransmit_queue_locked(struct wd_server* server) {
    if (!server || !server->net.retransmit_queue || !server->net.retransmit_queued)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    uint16_t out_count = 0;
    for (uint16_t i = 0; i < net->retransmit_queue_count; ++i)
    {
        const uint16_t tile_id = net->retransmit_queue[i];
        if (tile_id < server->total_tiles && net->retransmit_queued[tile_id])
        {
            net->retransmit_queue[out_count++] = tile_id;
        }
    }
    net->retransmit_queue_count = out_count;
}


static bool wd_stream_has_sendable_retransmit_locked(struct wd_server* server) {
    if (!server || !server->net.retransmit_queue || !server->net.retransmit_queued)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    for (uint16_t i = 0; i < net->retransmit_queue_count; ++i)
    {
        const uint16_t tile_id = net->retransmit_queue[i];
        if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
        {
            continue;
        }
        if (net->dirty_queued && net->dirty_queued[tile_id])
        {
            return true;
        }
        const uint64_t requested_generation = net->retransmit_requested_generation ? net->retransmit_requested_generation[tile_id] : 0;
        if (requested_generation == 0 || net->tiles[tile_id].generation >= requested_generation)
        {
            return true;
        }
    }
    return false;
}

static bool wd_stream_region_list_contains(const uint16_t* regions, uint16_t count, uint16_t region_id) {
    if (!regions)
    {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i)
    {
        if (regions[i] == region_id)
        {
            return true;
        }
    }
    return false;
}

static bool wd_stream_update_base_tile_metadata_locked(struct wd_server* server, uint16_t base_tile_id, uint64_t generation,
                                                       uint64_t timestamp_ns, uint64_t input_sequence) {
    if (!server || base_tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_tile_state* tile = &server->net.tiles[base_tile_id];
    tile->generation = generation;
    tile->timestamp_ns = timestamp_ns;
    tile->input_sequence = input_sequence;
    return true;
}


static void wd_stream_init_encode_job_locked(struct wd_parallel_encode_job* job, struct wd_server* server,
                                             uint16_t top_region_id, uint64_t input_sequence,
                                             uint64_t remaining_byte_budget, bool network_happy,
                                             const bool* dirty_snapshot, const uint64_t* epoch_snapshot,
                                             struct wd_parallel_encode_result* result, uint16_t result_capacity) {
    if (!job || !server)
    {
        return;
    }

    job->server = server;
    job->framebuffer_xrgb8888 = server->framebuffer_xrgb8888;
    job->display_width = server->display_width;
    job->display_height = server->display_height;
    job->tile_width = server->tile_width;
    job->tile_height = server->tile_height;
    job->tiles_x = server->tiles_x;
    job->tiles_y = server->tiles_y;
    job->total_tiles = server->total_tiles;
    job->top_region_id = top_region_id;
    job->input_sequence = input_sequence;
    job->remaining_byte_budget = remaining_byte_budget;
    job->framebuffer_generation = server->framebuffer_generation;
    job->udp_payload_target = server->net.udp_payload_target;
    job->network_happy = network_happy;
    job->dirty_snapshot = dirty_snapshot;
    job->dirty_epoch_snapshot = epoch_snapshot;
    job->result = result;
    job->result_capacity = result_capacity;
}

static bool wd_stream_encode_result_stale_locked(const struct wd_server* server, const struct wd_parallel_encode_result* result) {
    if (!server || !result || result->framebuffer_generation != server->framebuffer_generation)
    {
        return true;
    }

    const struct wd_net_state* net = &server->net;
    for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
    {
        const uint16_t covered_id = result->candidate.covered_base_ids[i];
        if (covered_id >= server->total_tiles)
        {
            return true;
        }
        if (net->dirty_epochs && net->dirty_epochs[covered_id] != result->covered_dirty_epochs[i])
        {
            return true;
        }
    }
    return false;
}

static uint64_t wd_stream_next_generation_for_result_locked(const struct wd_server* server,
                                                            const struct wd_parallel_encode_result* result) {
    uint64_t next_generation = 1;
    if (!server || !result)
    {
        return next_generation;
    }

    const struct wd_net_state* net = &server->net;
    for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
    {
        const uint16_t covered_id = result->candidate.covered_base_ids[i];
        if (covered_id < server->total_tiles && net->tiles[covered_id].generation >= next_generation)
        {
            next_generation = net->tiles[covered_id].generation + 1;
        }
    }
    return next_generation;
}

static void wd_stream_requeue_dirty_top_region_locked(struct wd_server* server, uint16_t top_region_id) {
    if (!server || !wd_stream_top_region_still_dirty_locked(server, top_region_id))
    {
        return;
    }

    uint16_t ids[64];
    uint16_t count = 0;
    if (wd_stream_collect_wire_tile_base_ids(server, top_region_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT,
                                             ids, &count, (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
    {
        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
    }
}

static void wd_stream_free_encode_result_payload(struct wd_parallel_encode_result* result) {
    if (!result)
    {
        return;
    }
    free(result->payload);
    result->payload = NULL;
}

static void wd_stream_free_encode_result_payloads(struct wd_parallel_encode_result* results, uint16_t result_count) {
    if (!results)
    {
        return;
    }
    for (uint16_t i = 0; i < result_count; ++i)
    {
        wd_stream_free_encode_result_payload(&results[i]);
    }
}

static bool wd_stream_run_encode_batch_locked(struct wd_server* server, struct wd_parallel_encode_batch* batch) {
    if (!server || !batch)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    uint16_t worker_threads = 0;
    const uint64_t wait_start_ns = wd_now_ns();
    net->encoder_batch_active = true;
    pthread_mutex_unlock(&net->lock);
    const bool encoded = wd_stream_encoder_pool_run(server, batch, &worker_threads);
    pthread_mutex_lock(&net->lock);
    net->encoder_batch_active = false;
    pthread_cond_broadcast(&net->encoder_idle_cond);
    net->stats.encode_wait_ns += wd_now_ns() - wait_start_ns;
    net->stats.encode_batches++;
    net->stats.encode_worker_threads += worker_threads;
    net->stats.encode_thread_wakeups += worker_threads;
    net->stats.encode_worker_ns += batch->worker_encode_ns;
    net->stats.tile_encode_ns += batch->worker_encode_ns;
    return encoded;
}

static void wd_stream_clear_retransmit_request_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (!net->retransmit_queued || !net->retransmit_queued[tile_id])
    {
        return;
    }

    net->retransmit_queued[tile_id] = false;
    if (net->retransmit_requested_generation)
    {
        net->retransmit_requested_generation[tile_id] = 0;
    }
    if (net->retransmit_queue_enqueued_ns)
    {
        wd_stream_note_queue_age_locked(net->retransmit_queue_enqueued_ns[tile_id], &net->stats.retx_queue_age_samples,
                                        &net->stats.retx_queue_age_sum_ns);
        net->retransmit_queue_enqueued_ns[tile_id] = 0;
    }
}

static void wd_stream_send_retransmits_locked(struct wd_server* server, uint64_t now) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (net->retransmit_queue_count == 0 || !net->retransmit_queued || !server->framebuffer_xrgb8888)
    {
        return;
    }

    while (net->retransmit_queue_count > 0)
    {
        uint64_t token_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
        if (token_budget == 0)
        {
            break;
        }

        bool* retx_snapshot = calloc((size_t)server->total_tiles, sizeof(*retx_snapshot));
        uint64_t* epoch_snapshot = malloc((size_t)server->total_tiles * sizeof(*epoch_snapshot));
        uint16_t* regions = calloc(net->retransmit_queue_count, sizeof(*regions));
        if (!retx_snapshot || !epoch_snapshot || !regions)
        {
            free(regions);
            free(epoch_snapshot);
            free(retx_snapshot);
            break;
        }

        if (net->dirty_epochs)
        {
            memcpy(epoch_snapshot, net->dirty_epochs, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }
        else
        {
            memset(epoch_snapshot, 0, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }

        uint16_t region_count = 0;
        for (uint16_t i = 0; i < net->retransmit_queue_count; ++i)
        {
            const uint16_t tile_id = net->retransmit_queue[i];
            if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
            {
                continue;
            }
            if (net->dirty_queued && net->dirty_queued[tile_id])
            {
                net->retransmit_queued[tile_id] = false;
                if (net->retransmit_queue_enqueued_ns)
                {
                    net->retransmit_queue_enqueued_ns[tile_id] = 0;
                }
                if (net->retransmit_requested_generation)
                {
                    net->retransmit_requested_generation[tile_id] = 0;
                }
                net->stats.retx_tiles_superseded_by_fresh++;
                continue;
            }

            const uint64_t requested_generation = net->retransmit_requested_generation ? net->retransmit_requested_generation[tile_id] : 0;
            if (requested_generation != 0 && net->tiles[tile_id].generation < requested_generation)
            {
                /* The request references a generation that has not reached this
                 * tile state yet. Keep it queued so a later fresh update can
                 * either satisfy it or supersede it instead of dropping the
                 * client's repair request. */
                continue;
            }

            uint16_t region_id = 0;
            if (!wd_stream_top_region_for_base_tile(server, tile_id, &region_id))
            {
                continue;
            }
            retx_snapshot[tile_id] = true;
            if (!wd_stream_region_list_contains(regions, region_count, region_id))
            {
                regions[region_count++] = region_id;
            }
        }

        wd_stream_compact_retransmit_queue_locked(server);
        if (region_count == 0)
        {
            free(regions);
            free(epoch_snapshot);
            free(retx_snapshot);
            break;
        }

        const uint64_t retx_input_sequence = net->input_since_last_fresh_tile ? net->last_input_sequence : 0;
        const bool network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);

        uint16_t batch_capacity = region_count;
        if (batch_capacity > (uint16_t)WD_ENCODER_BATCH_JOBS)
        {
            batch_capacity = (uint16_t)WD_ENCODER_BATCH_JOBS;
        }
        if (batch_capacity == 0)
        {
            free(regions);
            free(epoch_snapshot);
            free(retx_snapshot);
            break;
        }

        struct wd_parallel_encode_job* jobs = calloc(batch_capacity, sizeof(*jobs));
        struct wd_parallel_encode_result* results = calloc((size_t)batch_capacity * WD_ENCODER_MAX_RESULTS_PER_JOB, sizeof(*results));
        if (!jobs || !results)
        {
            free(results);
            free(jobs);
            free(regions);
            free(epoch_snapshot);
            free(retx_snapshot);
            break;
        }

        uint16_t job_count = 0;
        for (uint16_t i = 0; i < region_count && job_count < batch_capacity; ++i)
        {
            wd_stream_init_encode_job_locked(&jobs[job_count], server, regions[i], retx_input_sequence,
                                             token_budget, network_happy, retx_snapshot, epoch_snapshot,
                                             &results[(size_t)job_count * WD_ENCODER_MAX_RESULTS_PER_JOB],
                                             (uint16_t)WD_ENCODER_MAX_RESULTS_PER_JOB);
            job_count++;
        }

        if (job_count == 0)
        {
            free(results);
            free(jobs);
            free(regions);
            free(epoch_snapshot);
            free(retx_snapshot);
            break;
        }

        struct wd_parallel_encode_batch batch;
        memset(&batch, 0, sizeof(batch));
        batch.jobs = jobs;
        batch.job_count = job_count;

        const bool encoded = wd_stream_run_encode_batch_locked(server, &batch);

        bool stop_sending = !encoded;
        for (uint16_t ji = 0; ji < job_count && !stop_sending; ++ji)
        {
            const uint16_t result_count = jobs[ji].result_count != 0 ? jobs[ji].result_count : 1;
            for (uint16_t local_result = 0; local_result < result_count; ++local_result)
            {
                struct wd_parallel_encode_result* result = &jobs[ji].result[local_result];
                if (!result->valid)
                {
                    if (result->budget_blocked)
                    {
                        stop_sending = true;
                    }
                    break;
                }

                if (wd_stream_encode_result_stale_locked(server, result))
                {
                    net->stats.encode_jobs_stale++;
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                uint64_t current_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
                const bool current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
                if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget, current_network_happy))
                {
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                const uint64_t next_generation = wd_stream_next_generation_for_result_locked(server, result);

                struct wd_udp_tile_send_result send_result;
                if (!wd_stream_send_tile_payload_sized_locked(server, result->candidate.tile_id, result->candidate.width,
                                                              result->candidate.height, next_generation, now, retx_input_sequence,
                                                              result->payload, result->payload_size,
                                                              result->candidate.compressed_payload, &send_result))
                {
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                if (!send_result.all_packets_sent)
                {
                    if (send_result.any_packet_sent)
                    {
                        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                    }
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                wd_stream_note_tile_choice_locked(net, result->candidate.compressed_size, result->candidate.uncompressed_size,
                                                  net->udp_payload_target, retx_input_sequence,
                                                  result->candidate.compressed_payload, result->candidate.width, result->candidate.height);

                for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
                {
                    const uint16_t covered_id = result->candidate.covered_base_ids[i];
                    if (covered_id >= server->total_tiles)
                    {
                        continue;
                    }
                    (void)wd_stream_update_base_tile_metadata_locked(server, covered_id, next_generation, now, retx_input_sequence);
                    wd_stream_clear_retransmit_request_locked(server, covered_id);
                    if (net->dirty_epochs && net->dirty_epochs[covered_id] == result->covered_dirty_epochs[i])
                    {
                        wd_dirty_queue_note_cleared_locked(net, covered_id, server->total_tiles);
                        wd_stream_maybe_clear_dirty_top_region_for_base_locked(server, covered_id);
                    }
                    wd_stream_mark_summary_dirty_locked(server, covered_id);
                }

                net->stats.udp_retx_tiles_sent++;
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                wd_stream_free_encode_result_payload(result);

                if (send_result.send_blocked)
                {
                    stop_sending = true;
                    break;
                }
            }
        }

        wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_ENCODER_MAX_RESULTS_PER_JOB));
        free(results);
        free(jobs);
        free(regions);
        free(epoch_snapshot);
        free(retx_snapshot);
        wd_stream_compact_retransmit_queue_locked(server);

        if (stop_sending)
        {
            break;
        }
    }
}

bool wd_stream_send_dirty_tiles(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    if (!server->framebuffer_xrgb8888)
    {
        return false;
    }

    const uint64_t now = wd_now_ns();

    pthread_mutex_lock(&net->lock);

    if (!net->client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        return true;
    }

    if (net->config_update_pending)
    {
        pthread_mutex_unlock(&net->lock);
        return true;
    }

    /*
     * Normal send pass. New/reconnected clients are handled through the same
     * dirty-tile pipeline by invalidating tile state and marking the
     * scene dirty. Avoid a separate full-frame catch-up path, because it
     * competes with current dirty tiles and explicit retransmits for the same
     * byte budget.
     */
    const uint64_t dirty_detect_start_ns = wd_now_ns();
    wd_detect_dirty_tiles_into_queue_locked(server);
    net->stats.dirty_detect_ns += wd_now_ns() - dirty_detect_start_ns;

    const bool client_loss = wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
    if (client_loss && net->retransmit_queue_count > 0)
    {
        wd_stream_send_retransmits_locked(server, now);
    }

    while (net->dirty_region_count > 0)
    {
        const uint64_t remaining_byte_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
        const uint64_t tile_input_sequence = net->input_since_last_fresh_tile ? net->last_input_sequence : 0;
        const uint32_t top_uncompressed_bytes = WD_WIRE_TILE_MAX_WIDTH * WD_WIRE_TILE_MAX_HEIGHT * WD_BYTES_PER_PIXEL;
        const uint32_t minimum_wire_bytes = wd_stream_tile_wire_bytes_for_payload(top_uncompressed_bytes,
                                                                                  net->udp_payload_target, tile_input_sequence, false);
        if (minimum_wire_bytes == 0 || minimum_wire_bytes > remaining_byte_budget)
        {
            net->stats.dirty_budget_blocked++;
            break;
        }

        uint16_t batch_capacity = (uint16_t)WD_ENCODER_BATCH_JOBS;
        if (batch_capacity > net->dirty_region_count)
        {
            batch_capacity = net->dirty_region_count;
        }
        if (batch_capacity == 0)
        {
            break;
        }

        bool* dirty_snapshot = malloc((size_t)server->total_tiles * sizeof(*dirty_snapshot));
        uint64_t* epoch_snapshot = malloc((size_t)server->total_tiles * sizeof(*epoch_snapshot));
        struct wd_parallel_encode_job* jobs = calloc(batch_capacity, sizeof(*jobs));
        struct wd_parallel_encode_result* results = calloc((size_t)batch_capacity * WD_ENCODER_MAX_RESULTS_PER_JOB, sizeof(*results));
        if (!dirty_snapshot || !epoch_snapshot || !jobs || !results)
        {
            free(results);
            free(jobs);
            free(epoch_snapshot);
            free(dirty_snapshot);
            break;
        }

        memcpy(dirty_snapshot, net->dirty_queued, (size_t)server->total_tiles * sizeof(*dirty_snapshot));
        if (net->dirty_epochs)
        {
            memcpy(epoch_snapshot, net->dirty_epochs, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }
        else
        {
            memset(epoch_snapshot, 0, (size_t)server->total_tiles * sizeof(*epoch_snapshot));
        }

        const bool network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
        if (net->dirty_region_rng == 0)
        {
            net->dirty_region_rng = (uint32_t)(wd_now_ns() ^ ((uint64_t)server->display_width << 16) ^ server->display_height);
            if (net->dirty_region_rng == 0)
            {
                net->dirty_region_rng = 1;
            }
        }

        uint16_t job_count = 0;
        while (job_count < batch_capacity && net->dirty_region_count > 0)
        {
            const uint64_t select_start_ns = wd_now_ns();
            uint32_t r = net->dirty_region_rng = net->dirty_region_rng * 1664525u + 1013904223u;
            uint16_t index = (uint16_t)(r % net->dirty_region_count);
            uint16_t top_id = net->dirty_regions[index];
            if (!wd_stream_top_region_still_dirty_locked(server, top_id))
            {
                wd_stream_remove_dirty_top_region_locked(server, top_id);
                net->stats.dirty_region_select_ns += wd_now_ns() - select_start_ns;
                continue;
            }
            wd_stream_remove_dirty_top_region_locked(server, top_id);
            net->stats.dirty_region_select_ns += wd_now_ns() - select_start_ns;

            wd_stream_init_encode_job_locked(&jobs[job_count], server, top_id, tile_input_sequence,
                                             remaining_byte_budget, network_happy, dirty_snapshot, epoch_snapshot,
                                             &results[(size_t)job_count * WD_ENCODER_MAX_RESULTS_PER_JOB],
                                             (uint16_t)WD_ENCODER_MAX_RESULTS_PER_JOB);
            job_count++;
        }

        if (job_count == 0)
        {
            free(results);
            free(jobs);
            free(epoch_snapshot);
            free(dirty_snapshot);
            break;
        }

        net->stats.encode_jobs_submitted += job_count;

        struct wd_parallel_encode_batch batch;
        memset(&batch, 0, sizeof(batch));
        batch.jobs = jobs;
        batch.job_count = job_count;

        const bool encoded = wd_stream_run_encode_batch_locked(server, &batch);
        if (!encoded)
        {
            for (uint16_t ri = 0; ri < job_count; ++ri)
            {
                const uint16_t top_id = jobs[ri].top_region_id;
                if (wd_stream_top_region_still_dirty_locked(server, top_id))
                {
                    uint16_t ids[64];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, top_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
            }
            free(results);
            free(jobs);
            free(epoch_snapshot);
            free(dirty_snapshot);
            break;
        }

        bool stop_sending = false;
        for (uint16_t ji = 0; ji < job_count && !stop_sending; ++ji)
        {
            const uint16_t result_count = jobs[ji].result_count != 0 ? jobs[ji].result_count : 1;
            for (uint16_t local_result = 0; local_result < result_count; ++local_result)
            {
                struct wd_parallel_encode_result* result = &jobs[ji].result[local_result];
                if (!result->valid)
                {
                    if (result->budget_blocked && net->dirty_region_count > 0)
                    {
                        net->stats.dirty_budget_blocked++;
                        stop_sending = true;
                    }
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    break;
                }

                if (wd_stream_encode_result_stale_locked(server, result))
                {
                    net->stats.encode_jobs_stale++;
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                const uint64_t current_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
                const bool current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
                if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget, current_network_happy))
                {
                    net->stats.dirty_budget_blocked++;
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                const uint64_t next_generation = wd_stream_next_generation_for_result_locked(server, result);

                struct wd_udp_tile_send_result send_result;
                if (!wd_stream_send_tile_payload_sized_locked(server, result->candidate.tile_id, result->candidate.width,
                                                              result->candidate.height, next_generation, now, tile_input_sequence,
                                                              result->payload, result->payload_size,
                                                              result->candidate.compressed_payload, &send_result))
                {
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    continue;
                }

                if (!send_result.all_packets_sent)
                {
                    if (send_result.any_packet_sent)
                    {
                        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                    }
                    wd_stream_requeue_dirty_top_region_locked(server, result->top_region_id);
                    wd_stream_free_encode_result_payload(result);
                    stop_sending = true;
                    break;
                }

                wd_stream_note_tile_choice_locked(net, result->candidate.compressed_size, result->candidate.uncompressed_size,
                                                  net->udp_payload_target, tile_input_sequence,
                                                  result->candidate.compressed_payload, result->candidate.width, result->candidate.height);

                for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
                {
                    const uint16_t covered_id = result->candidate.covered_base_ids[i];
                    if (covered_id >= server->total_tiles)
                    {
                        continue;
                    }
                    (void)wd_stream_update_base_tile_metadata_locked(server, covered_id, next_generation, now, tile_input_sequence);
                    if (!net->dirty_epochs || net->dirty_epochs[covered_id] == result->covered_dirty_epochs[i])
                    {
                        wd_dirty_queue_note_cleared_locked(net, covered_id, server->total_tiles);
                    }
                    wd_stream_maybe_clear_dirty_top_region_for_base_locked(server, covered_id);
                    wd_stream_mark_summary_dirty_locked(server, covered_id);
                }

                net->stats.dirty_tiles++;
                net->stats.udp_fresh_tiles_sent++;
                net->stats.encode_jobs_completed++;
                wd_stream_note_input_to_first_fresh_tile_locked(net, now);
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                wd_stream_free_encode_result_payload(result);

                if (send_result.send_blocked)
                {
                    stop_sending = true;
                    break;
                }
            }
        }

        for (uint16_t ri = 0; ri < job_count; ++ri)
        {
            const uint16_t top_id = jobs[ri].top_region_id;
            wd_stream_requeue_dirty_top_region_locked(server, top_id);
        }

        wd_stream_free_encode_result_payloads(results, (uint16_t)(job_count * WD_ENCODER_MAX_RESULTS_PER_JOB));
        free(results);
        free(jobs);
        free(epoch_snapshot);
        free(dirty_snapshot);

        if (stop_sending)
        {
            break;
        }
    }

    wd_stream_send_retransmits_locked(server, now);

    /* Keep scene_dirty true only for work that can make progress now.
     * Future-generation retransmits stay queued, but a later fresh dirty
     * update will wake the stream path when they become actionable. */
    server->scene_dirty = net->dirty_region_count > 0 || wd_stream_has_sendable_retransmit_locked(server);

    pthread_mutex_unlock(&net->lock);

    return true;
}


struct wd_summary_completion_entry {
    uint16_t tile_id;
    uint64_t tile_generation;
};

struct wd_summary_completion {
    struct wd_server* server;
    bool full_summary;
    bool async_pending;
    bool input_since_last_summary;
    uint64_t server_timestamp_ns;
    uint64_t last_input_inject_ns;
    uint64_t summary_epoch;
    uint64_t budget_bytes;
    uint16_t entry_count;
    struct wd_summary_completion_entry entries[];
};

static void wd_stream_rebuild_summary_dirty_queue_locked(struct wd_server* server) {
    struct wd_net_state* net = &server->net;
    if (!net->summary_dirty_tiles || !net->summary_dirty_queue)
    {
        net->summary_dirty_count = 0;
        return;
    }

    uint16_t count = 0;
    for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
    {
        if (net->summary_dirty_tiles[tile_id])
        {
            net->summary_dirty_queue[count++] = tile_id;
        }
    }
    net->summary_dirty_count = count;
}

static void wd_stream_summary_completion(void* user_data, bool success) {
    struct wd_summary_completion* completion = user_data;
    if (!completion)
    {
        return;
    }

    if (completion->server)
    {
        struct wd_server* server = completion->server;
        struct wd_net_state* net = &server->net;

        if (completion->async_pending && net->summary_async_pending_count > 0)
        {
            net->summary_async_pending_count--;
            if (net->summary_async_pending_count == 0)
            {
                net->summary_async_pending_full = false;
            }
        }

        if (!success && completion->budget_bytes != 0)
        {
            wd_stream_refund_tcp_control_budget_locked(net, completion->budget_bytes);
            completion->budget_bytes = 0;
        }

        if (success && net->summary_dirty_tiles && net->summary_epoch == completion->summary_epoch)
        {
            for (uint16_t i = 0; i < completion->entry_count; ++i)
            {
                uint16_t tile_id = completion->entries[i].tile_id;
                if (tile_id < server->total_tiles && net->tiles[tile_id].generation == completion->entries[i].tile_generation)
                {
                    net->summary_dirty_tiles[tile_id] = false;
                }
            }
            wd_stream_rebuild_summary_dirty_queue_locked(server);
        }

        if (success && completion->input_since_last_summary && completion->last_input_inject_ns != 0 &&
            completion->server_timestamp_ns >= completion->last_input_inject_ns)
        {
            net->stats.input_to_summary_samples++;
            net->stats.input_to_summary_sum_ns += completion->server_timestamp_ns - completion->last_input_inject_ns;
            net->input_since_last_summary = false;
        }
    }

    free(completion);
}

static bool wd_stream_send_generation_summary_kind_locked(struct wd_server* server, bool full_summary) {
    struct wd_net_state* net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0)
    {
        return true;
    }

    if (net->config_update_pending)
    {
        return false;
    }

    if (net->control_tx && net->summary_async_pending_count != 0)
    {
        if (!full_summary && net->summary_async_pending_full)
        {
            return false;
        }

        uint32_t dropped = wd_async_tcp_sender_drop_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY);
        if (dropped != 0)
        {
            net->stats.tcp_summary_coalesced += dropped;
        }
        if (wd_async_tcp_sender_has_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY))
        {
            return false;
        }
    }

    const uint64_t summary_build_start_ns = wd_now_ns();
    uint16_t requested_tile_count = full_summary ? server->total_tiles : net->summary_dirty_count;
    if (requested_tile_count == 0)
    {
        return true;
    }

    size_t payload_capacity = sizeof(struct wd_tile_summary_payload_header) +
                              (size_t)requested_tile_count * sizeof(struct wd_tile_generation_entry);

    uint8_t* payload = malloc(payload_capacity);
    if (!payload)
    {
        return false;
    }

    struct wd_tile_generation_entry* entries =
        (struct wd_tile_generation_entry*)(payload + sizeof(struct wd_tile_summary_payload_header));

    uint16_t entry_count = 0;

    if (full_summary)
    {
        for (uint16_t i = 0; i < server->total_tiles; ++i)
        {
            entries[entry_count].tile_id         = i;
            entries[entry_count].tile_generation = net->tiles[i].generation;
            entry_count++;
        }
    }
    else if (net->summary_dirty_tiles && net->summary_dirty_queue)
    {
        for (uint16_t queue_index = 0; queue_index < net->summary_dirty_count; ++queue_index)
        {
            uint16_t tile_id = net->summary_dirty_queue[queue_index];
            if (tile_id >= server->total_tiles || !net->summary_dirty_tiles[tile_id])
            {
                continue;
            }

            entries[entry_count].tile_id         = tile_id;
            entries[entry_count].tile_generation = net->tiles[tile_id].generation;
            entry_count++;
        }
    }

    if (entry_count == 0)
    {
        free(payload);
        net->summary_dirty_count = 0;
        net->stats.summary_build_ns += wd_now_ns() - summary_build_start_ns;
        return true;
    }

    struct wd_tile_summary_payload_header header;

    header.session_id          = net->session_id;
    header.server_timestamp_ns = wd_now_ns();
    header.tile_count          = entry_count;
    header.flags               = full_summary ? 0u : WD_TILE_SUMMARY_DELTA;

    memcpy(payload, &header, sizeof(header));

    size_t payload_size = sizeof(header) + (size_t)entry_count * sizeof(struct wd_tile_generation_entry);
    net->stats.summary_build_ns += wd_now_ns() - summary_build_start_ns;

    const uint64_t frame_size = (uint64_t)sizeof(struct wd_tcp_header) + (uint64_t)payload_size;
    if (frame_size > UINT32_MAX ||
        !wd_stream_try_consume_tcp_control_budget_locked(net, (uint32_t)frame_size, header.server_timestamp_ns))
    {
        free(payload);
        return false;
    }

    struct wd_summary_completion* completion = calloc(1, sizeof(*completion) +
                                                          (size_t)entry_count * sizeof(completion->entries[0]));
    if (!completion)
    {
        wd_stream_refund_tcp_control_budget_locked(net, frame_size);
        free(payload);
        return false;
    }
    completion->server = server;
    completion->full_summary = full_summary;
    completion->input_since_last_summary = net->input_since_last_summary;
    completion->server_timestamp_ns = header.server_timestamp_ns;
    completion->last_input_inject_ns = net->last_input_inject_ns;
    completion->summary_epoch = net->summary_epoch;
    completion->budget_bytes = frame_size;
    completion->entry_count = entry_count;
    for (uint16_t i = 0; i < entry_count; ++i)
    {
        completion->entries[i].tile_id = entries[i].tile_id;
        completion->entries[i].tile_generation = entries[i].tile_generation;
    }

    bool ok = false;
    if (net->control_tx)
    {
        ok = wd_async_tcp_send_message_ex(net->control_tx, net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload,
                                          (uint32_t)payload_size, wd_stream_summary_completion, completion);
        if (ok)
        {
            completion->async_pending = true;
            net->summary_async_pending_count++;
            if (full_summary)
            {
                net->summary_async_pending_full = true;
            }
        }
        if (!ok)
        {
            net->stats.tcp_async_send_failed++;
            if (net->tcp_fd >= 0)
            {
                (void)shutdown(net->tcp_fd, SHUT_RDWR);
            }
        }
    }
    else
    {
        ok = wd_send_tcp_message(net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload, (uint32_t)payload_size);
        wd_stream_summary_completion(completion, ok);
        completion = NULL;
    }

    if (ok)
    {
        if (full_summary)
        {
            net->stats.tcp_summary_full_tx++;
        }
        else
        {
            net->stats.tcp_summary_delta_tx++;
            net->stats.tcp_summary_delta_tiles += entry_count;
        }
        net->stats.tcp_summary_tx++;
    }

    if (!ok && completion)
    {
        wd_stream_summary_completion(completion, false);
    }

    free(payload);

    return ok;
}

bool wd_stream_send_generation_summary_locked(struct wd_server* server) {
    return wd_stream_send_generation_summary_kind_locked(server, true);
}

bool wd_stream_send_pending_generation_summary_locked(struct wd_server* server) {
    return wd_stream_send_generation_summary_kind_locked(server, false);
}

static void wd_stats_accumulate(struct wd_stats* dst, const struct wd_stats* src) {
    if (!dst || !src)
    {
        return;
    }

    dst->dirty_tiles += src->dirty_tiles;
    dst->udp_tiles_sent += src->udp_tiles_sent;
    dst->udp_fresh_tiles_sent += src->udp_fresh_tiles_sent;
    dst->udp_retx_tiles_sent += src->udp_retx_tiles_sent;
    dst->udp_compressed_tiles_sent += src->udp_compressed_tiles_sent;
    dst->udp_uncompressed_tiles_sent += src->udp_uncompressed_tiles_sent;
    dst->udp_compressed_tile_bytes_sent += src->udp_compressed_tile_bytes_sent;
    dst->udp_uncompressed_tile_bytes_sent += src->udp_uncompressed_tile_bytes_sent;
    dst->udp_packets_sent += src->udp_packets_sent;
    dst->udp_bytes_sent += src->udp_bytes_sent;
    dst->udp_send_pressure_drops += src->udp_send_pressure_drops;
    dst->tile_choice_compressed += src->tile_choice_compressed;
    dst->tile_choice_uncompressed += src->tile_choice_uncompressed;
    dst->tile_choice_compressed_payload_sum += src->tile_choice_compressed_payload_sum;
    dst->tile_choice_uncompressed_payload_sum += src->tile_choice_uncompressed_payload_sum;
    dst->tile_choice_compressed_wire_sum += src->tile_choice_compressed_wire_sum;
    dst->tile_choice_uncompressed_wire_sum += src->tile_choice_uncompressed_wire_sum;
    dst->tile_choice_chosen_wire_sum += src->tile_choice_chosen_wire_sum;
    dst->tile_choice_saved_wire_sum += src->tile_choice_saved_wire_sum;
    dst->tile_size_128x64_sent += src->tile_size_128x64_sent;
    dst->tile_size_64x64_sent += src->tile_size_64x64_sent;
    dst->tile_size_32x32_sent += src->tile_size_32x32_sent;
    dst->tile_size_16x16_sent += src->tile_size_16x16_sent;
    dst->tcp_hello_rx += src->tcp_hello_rx;
    dst->tcp_config_tx += src->tcp_config_tx;
    dst->tcp_summary_tx += src->tcp_summary_tx;
    dst->tcp_input_channel_rx += src->tcp_input_channel_rx;
    dst->tcp_input_channel_accepted += src->tcp_input_channel_accepted;
    dst->tcp_input_channel_closed += src->tcp_input_channel_closed;
    dst->tcp_selection_channel_rx += src->tcp_selection_channel_rx;
    dst->tcp_selection_channel_accepted += src->tcp_selection_channel_accepted;
    dst->tcp_selection_channel_closed += src->tcp_selection_channel_closed;
    dst->client_stats_rx += src->client_stats_rx;
    dst->client_udp_packets_rx += src->client_udp_packets_rx;
    dst->client_udp_bytes_rx += src->client_udp_bytes_rx;
    dst->client_tiles_completed += src->client_tiles_completed;
    dst->client_completed_packets += src->client_completed_packets;
    dst->client_partial_tiles_timed_out += src->client_partial_tiles_timed_out;
    dst->client_old_generation_tiles += src->client_old_generation_tiles;
    dst->client_retx_requests_tx += src->client_retx_requests_tx;
    dst->client_udp_interarrival_samples += src->client_udp_interarrival_samples;
    dst->client_udp_interarrival_sum_ns += src->client_udp_interarrival_sum_ns;
    dst->client_udp_interarrival_jitter_samples += src->client_udp_interarrival_jitter_samples;
    dst->client_udp_interarrival_jitter_sum_ns += src->client_udp_interarrival_jitter_sum_ns;
    dst->client_render_visible_reports += src->client_render_visible_reports;
    dst->client_render_hidden_reports += src->client_render_hidden_reports;
    if (src->client_udp_interarrival_max_ns > dst->client_udp_interarrival_max_ns)
    {
        dst->client_udp_interarrival_max_ns = src->client_udp_interarrival_max_ns;
    }
    dst->client_render_frames += src->client_render_frames;
    dst->client_present_samples += src->client_present_samples;
    dst->client_present_sum_ns += src->client_present_sum_ns;
    if (src->client_present_max_ns > dst->client_present_max_ns)
    {
        dst->client_present_max_ns = src->client_present_max_ns;
    }
    dst->client_input_present_samples += src->client_input_present_samples;
    dst->client_input_present_sum_ns += src->client_input_present_sum_ns;
    dst->retx_req_rx += src->retx_req_rx;
    dst->retx_tiles_req += src->retx_tiles_req;
    dst->retx_req_ignored_live += src->retx_req_ignored_live;
    dst->key_events_rx += src->key_events_rx;
    dst->key_events_injected += src->key_events_injected;
    dst->key_events_dropped += src->key_events_dropped;
    dst->key_state_duplicate_presses += src->key_state_duplicate_presses;
    dst->key_state_release_without_press += src->key_state_release_without_press;
    dst->keyboard_enter_events += src->keyboard_enter_events;
    dst->pointer_events_rx += src->pointer_events_rx;
    dst->pointer_events_injected += src->pointer_events_injected;
    dst->pointer_events_dropped += src->pointer_events_dropped;
    dst->pointer_button_grab_started += src->pointer_button_grab_started;
    dst->pointer_button_grab_ended += src->pointer_button_grab_ended;
    dst->pointer_button_grab_cleared += src->pointer_button_grab_cleared;
    dst->pointer_button_grab_surface_destroyed += src->pointer_button_grab_surface_destroyed;
    dst->xdg_move_invalid_serial += src->xdg_move_invalid_serial;
    dst->xdg_resize_invalid_serial += src->xdg_resize_invalid_serial;
    dst->popup_explicit_scene_trees += src->popup_explicit_scene_trees;
    dst->popup_explicit_scene_tree_failures += src->popup_explicit_scene_tree_failures;
    dst->cursor_shape_requests += src->cursor_shape_requests;
    dst->cursor_shape_tx += src->cursor_shape_tx;
    dst->cursor_shape_coalesced += src->cursor_shape_coalesced;
    dst->cursor_set_cursor_requests += src->cursor_set_cursor_requests;
    dst->cursor_set_cursor_rejected += src->cursor_set_cursor_rejected;
    dst->cursor_set_cursor_hidden += src->cursor_set_cursor_hidden;
    dst->cursor_set_cursor_fallback += src->cursor_set_cursor_fallback;
    dst->input_net_latency_samples += src->input_net_latency_samples;
    dst->input_net_latency_sum_ns += src->input_net_latency_sum_ns;
    dst->input_queue_latency_samples += src->input_queue_latency_samples;
    dst->input_queue_latency_sum_ns += src->input_queue_latency_sum_ns;
    dst->input_to_summary_samples += src->input_to_summary_samples;
    dst->input_to_summary_sum_ns += src->input_to_summary_sum_ns;
    dst->input_to_first_fresh_tile_samples += src->input_to_first_fresh_tile_samples;
    dst->input_to_first_fresh_tile_sum_ns += src->input_to_first_fresh_tile_sum_ns;
    dst->tcp_summary_full_tx += src->tcp_summary_full_tx;
    dst->tcp_summary_delta_tx += src->tcp_summary_delta_tx;
    dst->tcp_summary_delta_tiles += src->tcp_summary_delta_tiles;
    dst->tcp_summary_coalesced += src->tcp_summary_coalesced;
    dst->tcp_summary_budget_interval_ns += src->tcp_summary_budget_interval_ns;
    dst->tcp_summary_repair_backoff += src->tcp_summary_repair_backoff;
    dst->tcp_control_bytes_sent += src->tcp_control_bytes_sent;
    dst->tcp_control_bytes_refunded += src->tcp_control_bytes_refunded;
    dst->tcp_budget_blocked += src->tcp_budget_blocked;
    dst->tcp_async_send_failed += src->tcp_async_send_failed;
    dst->tcp_async_queue_overflow += src->tcp_async_queue_overflow;
    dst->tcp_async_queued += src->tcp_async_queued;
    dst->tcp_async_completed += src->tcp_async_completed;
    dst->tcp_async_completion_failed += src->tcp_async_completion_failed;
    dst->tcp_async_partial_resubmits += src->tcp_async_partial_resubmits;
    if (src->tcp_async_inflight_max > dst->tcp_async_inflight_max)
    {
        dst->tcp_async_inflight_max = src->tcp_async_inflight_max;
    }
    dst->udp_async_send_failed += src->udp_async_send_failed;
    dst->udp_async_queued += src->udp_async_queued;
    dst->udp_async_completed += src->udp_async_completed;
    dst->udp_async_completion_failed += src->udp_async_completion_failed;
    dst->udp_async_fallback_sync += src->udp_async_fallback_sync;
    if (src->udp_async_inflight_max > dst->udp_async_inflight_max)
    {
        dst->udp_async_inflight_max = src->udp_async_inflight_max;
    }
    dst->rate_decreases += src->rate_decreases;
    dst->rate_increases += src->rate_increases;
    dst->frame_rate_downshifts += src->frame_rate_downshifts;
    dst->frame_rate_upshifts += src->frame_rate_upshifts;
    dst->dirty_region_probes += src->dirty_region_probes;
    dst->dirty_region_hits += src->dirty_region_hits;
    dst->dirty_budget_blocked += src->dirty_budget_blocked;
    dst->partial_tile_sends += src->partial_tile_sends;
    dst->partial_tile_packets_sent += src->partial_tile_packets_sent;
    dst->dirty_detect_ns += src->dirty_detect_ns;
    dst->dirty_region_select_ns += src->dirty_region_select_ns;
    dst->tile_encode_ns += src->tile_encode_ns;
    dst->summary_build_ns += src->summary_build_ns;
    dst->udp_send_ns += src->udp_send_ns;
    dst->encode_jobs_submitted += src->encode_jobs_submitted;
    dst->encode_jobs_completed += src->encode_jobs_completed;
    dst->encode_jobs_stale += src->encode_jobs_stale;
    dst->encode_worker_ns += src->encode_worker_ns;
    dst->encode_wait_ns += src->encode_wait_ns;
    dst->encode_batches += src->encode_batches;
    dst->encode_worker_threads += src->encode_worker_threads;
    dst->encode_thread_wakeups += src->encode_thread_wakeups;
    dst->dirty_tiles_stale_skipped += src->dirty_tiles_stale_skipped;
    dst->retx_tiles_superseded_by_fresh += src->retx_tiles_superseded_by_fresh;
    dst->dirty_queue_age_samples += src->dirty_queue_age_samples;
    dst->dirty_queue_age_sum_ns += src->dirty_queue_age_sum_ns;
    dst->retx_queue_age_samples += src->retx_queue_age_samples;
    dst->retx_queue_age_sum_ns += src->retx_queue_age_sum_ns;
    dst->retx_req_stale_generation += src->retx_req_stale_generation;
}

static double wd_avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return (double)sum_ns / (double)samples / 1000000.0;
}

void wd_stream_sample_and_maybe_log_stats(struct wd_server* server, bool log_stats) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stats s = net->stats;
    memset(&net->stats, 0, sizeof(net->stats));
    wd_stream_policy_update_health_locked(&net->stream_policy, &s);
    uint64_t limited_udp_kib_per_second = net->stream_policy.limited_udp_bytes_per_second / 1024ull;
    uint16_t target_fps = net->stream_policy.target_fps;
    uint16_t effective_target_fps = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    uint16_t output_fps = wd_stream_policy_output_fps_locked(&net->stream_policy);
    bool client_render_visible = net->stream_policy.client_render_visible;
    uint16_t tile_width = server->tile_width;
    uint16_t tile_height = server->tile_height;
    bool input_channel_connected = net->input_tcp_fd >= 0;
    bool selection_channel_connected = net->selection_tcp_fd >= 0;

    pthread_mutex_unlock(&net->lock);

    struct wd_stats_log_state* stats_log = &server->stats_log;
    wd_stats_accumulate(&stats_log->totals, &s);
    if (!log_stats)
    {
        return;
    }

    s = stats_log->totals;
    memset(&stats_log->totals, 0, sizeof(stats_log->totals));

    bool state_changed = !stats_log->have_prev_state ||
                         stats_log->prev_target_fps != target_fps || stats_log->prev_effective_fps != effective_target_fps ||
                         stats_log->prev_output_fps != output_fps || stats_log->prev_client_render_visible != client_render_visible ||
                         stats_log->prev_limited_kib != limited_udp_kib_per_second || stats_log->prev_tile_width != tile_width ||
                         stats_log->prev_tile_height != tile_height || stats_log->prev_input_channel != input_channel_connected ||
                         stats_log->prev_selection_channel != selection_channel_connected;

    if (state_changed)
    {
        WD_LOG_DEBUG("state: target_fps=%u effective_fps=%u output_fps=%u client_visible=%s udp_budget_kib_per_sec=%llu base_tile=%ux%u wire_tiles=128x64,64x64,32x32,16x16 input_channel=%s selection_channel=%s",
                     (unsigned)target_fps, (unsigned)effective_target_fps, (unsigned)output_fps,
                     client_render_visible ? "yes" : "no",
                     (unsigned long long)limited_udp_kib_per_second, (unsigned)tile_width, (unsigned)tile_height,
                     input_channel_connected ? "yes" : "no", selection_channel_connected ? "yes" : "no");

        stats_log->have_prev_state = true;
        stats_log->prev_target_fps = target_fps;
        stats_log->prev_effective_fps = effective_target_fps;
        stats_log->prev_output_fps = output_fps;
        stats_log->prev_client_render_visible = client_render_visible;
        stats_log->prev_limited_kib = limited_udp_kib_per_second;
        stats_log->prev_tile_width = tile_width;
        stats_log->prev_tile_height = tile_height;
        stats_log->prev_input_channel = input_channel_connected;
        stats_log->prev_selection_channel = selection_channel_connected;
    }

    bool video_activity = s.dirty_tiles != 0 || s.dirty_tiles_stale_skipped != 0 || s.udp_tiles_sent != 0 ||
                          s.udp_fresh_tiles_sent != 0 || s.udp_retx_tiles_sent != 0 || s.udp_packets_sent != 0 ||
                          s.udp_bytes_sent != 0 || s.udp_send_pressure_drops != 0 || s.udp_async_send_failed != 0 ||
                          s.udp_async_queued != 0 || s.udp_async_completed != 0 ||
                          s.udp_async_completion_failed != 0 || s.udp_async_fallback_sync != 0 ||
                          s.tile_choice_compressed != 0 || s.tile_choice_uncompressed != 0 ||
                          s.dirty_queue_age_samples != 0 || s.retx_queue_age_samples != 0 ||
                          s.dirty_region_probes != 0 || s.dirty_region_hits != 0 ||
                          s.dirty_budget_blocked != 0 || s.partial_tile_sends != 0 || s.dirty_detect_ns != 0 || s.dirty_region_select_ns != 0 ||
                          s.tile_encode_ns != 0 || s.summary_build_ns != 0 || s.udp_send_ns != 0 || s.encode_jobs_submitted != 0 ||
                          s.encode_jobs_completed != 0 || s.encode_jobs_stale != 0 || s.encode_wait_ns != 0 || s.encode_batches != 0;
    if (video_activity)
    {
        uint64_t choices = s.tile_choice_compressed + s.tile_choice_uncompressed;
        WD_LOG_DEBUG("video/min: dirty=%llu stale_skip=%llu udp_tiles=%llu fresh=%llu retx=%llu pkts=%llu kib=%.1f wire_avg_B=%.1f comp_sent=%llu uncomp_sent=%llu comp_payload_avg_B=%.1f uncomp_payload_avg_B=%.1f choice_comp=%llu choice_uncomp=%llu choice_comp_payload_avg_B=%.1f choice_raw_payload_avg_B=%.1f choice_comp_wire_avg_B=%.1f choice_uncomp_wire_avg_B=%.1f choice_chosen_wire_avg_B=%.1f choice_saved_kib=%.1f pressure_drops=%llu async_queued=%llu async_completed=%llu async_failed=%llu async_completion_failed=%llu async_fallback=%llu async_inflight_max=%llu dirty_q_avg_ms=%.2f retx_q_avg_ms=%.2f dirty_region_probes=%llu dirty_region_hits=%llu dirty_budget_blocked=%llu partial_tiles=%llu partial_pkts=%llu detect_ms=%.2f region_pick_ms=%.2f encode_ms=%.2f udp_send_ms=%.2f summary_ms=%.2f tile_sizes=128x64:%llu,64x64:%llu,32x32:%llu,16x16:%llu encode_jobs=%llu/%llu stale=%llu encode_wait_ms=%.2f encode_worker_ms=%.2f encode_batches=%llu encode_workers_avg=%.1f encode_wakeups=%llu",
                     (unsigned long long)s.dirty_tiles, (unsigned long long)s.dirty_tiles_stale_skipped,
                     (unsigned long long)s.udp_tiles_sent, (unsigned long long)s.udp_fresh_tiles_sent,
                     (unsigned long long)s.udp_retx_tiles_sent, (unsigned long long)s.udp_packets_sent,
                     (double)s.udp_bytes_sent / 1024.0,
                     s.udp_tiles_sent ? (double)s.udp_bytes_sent / (double)s.udp_tiles_sent : 0.0,
                     (unsigned long long)s.udp_compressed_tiles_sent, (unsigned long long)s.udp_uncompressed_tiles_sent,
                     s.udp_compressed_tiles_sent
                         ? (double)s.udp_compressed_tile_bytes_sent / (double)s.udp_compressed_tiles_sent
                         : 0.0,
                     s.udp_uncompressed_tiles_sent
                         ? (double)s.udp_uncompressed_tile_bytes_sent / (double)s.udp_uncompressed_tiles_sent
                         : 0.0,
                     (unsigned long long)s.tile_choice_compressed, (unsigned long long)s.tile_choice_uncompressed,
                     choices ? (double)s.tile_choice_compressed_payload_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_uncompressed_payload_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_compressed_wire_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_uncompressed_wire_sum / (double)choices : 0.0,
                     choices ? (double)s.tile_choice_chosen_wire_sum / (double)choices : 0.0,
                     (double)s.tile_choice_saved_wire_sum / 1024.0,
                     (unsigned long long)s.udp_send_pressure_drops,
                     (unsigned long long)s.udp_async_queued,
                     (unsigned long long)s.udp_async_completed,
                     (unsigned long long)s.udp_async_send_failed,
                     (unsigned long long)s.udp_async_completion_failed,
                     (unsigned long long)s.udp_async_fallback_sync,
                     (unsigned long long)s.udp_async_inflight_max,
                     wd_avg_ms(s.dirty_queue_age_sum_ns, s.dirty_queue_age_samples),
                     wd_avg_ms(s.retx_queue_age_sum_ns, s.retx_queue_age_samples),
                     (unsigned long long)s.dirty_region_probes,
                     (unsigned long long)s.dirty_region_hits,
                     (unsigned long long)s.dirty_budget_blocked,
                     (unsigned long long)s.partial_tile_sends,
                     (unsigned long long)s.partial_tile_packets_sent,
                     (double)s.dirty_detect_ns / 1000000.0,
                     (double)s.dirty_region_select_ns / 1000000.0,
                     (double)s.tile_encode_ns / 1000000.0,
                     (double)s.udp_send_ns / 1000000.0,
                     (double)s.summary_build_ns / 1000000.0,
                     (unsigned long long)s.tile_size_128x64_sent,
                     (unsigned long long)s.tile_size_64x64_sent,
                     (unsigned long long)s.tile_size_32x32_sent,
                     (unsigned long long)s.tile_size_16x16_sent,
                     (unsigned long long)s.encode_jobs_completed,
                     (unsigned long long)s.encode_jobs_submitted,
                     (unsigned long long)s.encode_jobs_stale,
                     (double)s.encode_wait_ns / 1000000.0,
                     (double)s.encode_worker_ns / 1000000.0,
                     (unsigned long long)s.encode_batches,
                     s.encode_batches ? (double)s.encode_worker_threads / (double)s.encode_batches : 0.0,
                     (unsigned long long)s.encode_thread_wakeups);
    }

    bool repair_activity = s.retx_req_rx != 0 || s.retx_tiles_req != 0 || s.retx_req_ignored_live != 0 ||
                           s.retx_req_stale_generation != 0 || s.retx_tiles_superseded_by_fresh != 0 ||
                           s.tcp_summary_tx != 0 || s.tcp_summary_delta_tx != 0 ||
                           s.tcp_summary_delta_tiles != 0 || s.tcp_summary_coalesced != 0 ||
                           s.tcp_summary_repair_backoff != 0 || s.tcp_summary_budget_interval_ns != 0 ||
                           s.rate_decreases != 0 || s.rate_increases != 0 ||
                           s.frame_rate_downshifts != 0 || s.frame_rate_upshifts != 0;
    if (repair_activity)
    {
        WD_LOG_DEBUG("repair/min: summaries=%llu full=%llu delta=%llu delta_tiles=%llu summary_coalesced=%llu summary_interval_ms=%llu repair_backoff=%llu retx_req=%llu retx_tiles=%llu stale_gen=%llu ignored_live=%llu superseded=%llu rate_down=%llu rate_up=%llu fps_down=%llu fps_up=%llu",
                     (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.tcp_summary_full_tx,
                     (unsigned long long)s.tcp_summary_delta_tx, (unsigned long long)s.tcp_summary_delta_tiles,
                     (unsigned long long)s.tcp_summary_coalesced,
                     (unsigned long long)(s.tcp_summary_budget_interval_ns / 1000000ull),
                     (unsigned long long)s.tcp_summary_repair_backoff,
                     (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req,
                     (unsigned long long)s.retx_req_stale_generation,
                     (unsigned long long)s.retx_req_ignored_live, (unsigned long long)s.retx_tiles_superseded_by_fresh,
                     (unsigned long long)s.rate_decreases, (unsigned long long)s.rate_increases,
                     (unsigned long long)s.frame_rate_downshifts, (unsigned long long)s.frame_rate_upshifts);
    }

    bool client_activity = s.client_tiles_completed != 0 || s.client_udp_bytes_rx != 0 || s.client_partial_tiles_timed_out != 0 ||
                           s.client_old_generation_tiles != 0 || s.client_retx_requests_tx != 0 ||
                           s.client_udp_interarrival_samples != 0 || s.client_render_frames != 0 ||
                           s.client_present_samples != 0 || s.client_input_present_samples != 0 ||
                           s.client_render_visible_reports != 0 || s.client_render_hidden_reports != 0;
    if (client_activity)
    {
        WD_LOG_DEBUG("client/min: reports=%llu visible=%llu hidden=%llu completed=%llu udp_kib=%.1f partial_timeouts=%llu old_gen=%llu retx_req_tx=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f render_frames=%llu present_avg_ms=%.2f present_max_ms=%.2f input_present_avg_ms=%.2f",
                     (unsigned long long)s.client_stats_rx,
                     (unsigned long long)s.client_render_visible_reports,
                     (unsigned long long)s.client_render_hidden_reports,
                     (unsigned long long)s.client_tiles_completed,
                     (double)s.client_udp_bytes_rx / 1024.0, (unsigned long long)s.client_partial_tiles_timed_out,
                     (unsigned long long)s.client_old_generation_tiles, (unsigned long long)s.client_retx_requests_tx,
                     wd_avg_ms(s.client_udp_interarrival_sum_ns, s.client_udp_interarrival_samples),
                     wd_avg_ms(s.client_udp_interarrival_jitter_sum_ns, s.client_udp_interarrival_jitter_samples),
                     (double)s.client_udp_interarrival_max_ns / 1000000.0,
                     (unsigned long long)s.client_render_frames,
                     wd_avg_ms(s.client_present_sum_ns, s.client_present_samples),
                     (double)s.client_present_max_ns / 1000000.0,
                     wd_avg_ms(s.client_input_present_sum_ns, s.client_input_present_samples));
    }

    bool control_activity = s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.tcp_input_channel_rx != 0 ||
                            s.tcp_input_channel_accepted != 0 || s.tcp_input_channel_closed != 0 ||
                            s.tcp_selection_channel_rx != 0 || s.tcp_selection_channel_accepted != 0 ||
                            s.tcp_selection_channel_closed != 0 || s.tcp_async_send_failed != 0 ||
                            s.tcp_async_queued != 0 || s.tcp_async_completed != 0 ||
                            s.tcp_async_completion_failed != 0 || s.tcp_async_queue_overflow != 0 || s.tcp_async_partial_resubmits != 0 ||
                            s.tcp_control_bytes_sent != 0 || s.tcp_control_bytes_refunded != 0 || s.tcp_budget_blocked != 0;
    if (control_activity)
    {
        WD_LOG_DEBUG("control/min: hello=%llu config=%llu input_rx=%llu input_accepted=%llu input_closed=%llu selection_rx=%llu selection_accepted=%llu selection_closed=%llu async_queued=%llu async_completed=%llu async_send_failed=%llu async_completion_failed=%llu async_overflow=%llu async_partial=%llu async_inflight_max=%llu tcp_kib=%.1f tcp_refund_kib=%.1f tcp_budget_blocked=%llu",
                     (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                     (unsigned long long)s.tcp_input_channel_rx, (unsigned long long)s.tcp_input_channel_accepted,
                     (unsigned long long)s.tcp_input_channel_closed, (unsigned long long)s.tcp_selection_channel_rx,
                     (unsigned long long)s.tcp_selection_channel_accepted, (unsigned long long)s.tcp_selection_channel_closed,
                     (unsigned long long)s.tcp_async_queued,
                     (unsigned long long)s.tcp_async_completed,
                     (unsigned long long)s.tcp_async_send_failed,
                     (unsigned long long)s.tcp_async_completion_failed,
                     (unsigned long long)s.tcp_async_queue_overflow,
                     (unsigned long long)s.tcp_async_partial_resubmits,
                     (unsigned long long)s.tcp_async_inflight_max,
                     (double)s.tcp_control_bytes_sent / 1024.0,
                     (double)s.tcp_control_bytes_refunded / 1024.0,
                     (unsigned long long)s.tcp_budget_blocked);
    }

    bool input_activity = s.key_events_rx != 0 || s.key_events_injected != 0 || s.key_events_dropped != 0 ||
                          s.key_state_duplicate_presses != 0 || s.key_state_release_without_press != 0 ||
                          s.keyboard_enter_events != 0 || s.pointer_events_rx != 0 || s.pointer_events_injected != 0 ||
                          s.pointer_events_dropped != 0 || s.pointer_button_grab_started != 0 ||
                          s.pointer_button_grab_ended != 0 || s.pointer_button_grab_cleared != 0 ||
                          s.pointer_button_grab_surface_destroyed != 0 || s.input_queue_latency_samples != 0 ||
                          s.input_to_summary_samples != 0 || s.input_to_first_fresh_tile_samples != 0;
    if (input_activity)
    {
        WD_LOG_DEBUG("input/min: key_rx=%llu key_injected=%llu key_dropped=%llu dup_press=%llu release_without_press=%llu keyboard_enter=%llu pointer_rx=%llu pointer_injected=%llu pointer_dropped=%llu grabs_start=%llu grabs_end=%llu grabs_clear=%llu grab_surface_destroyed=%llu queue_avg_ms=%.2f input_to_summary_avg_ms=%.2f input_to_first_tile_avg_ms=%.2f",
                     (unsigned long long)s.key_events_rx, (unsigned long long)s.key_events_injected,
                     (unsigned long long)s.key_events_dropped, (unsigned long long)s.key_state_duplicate_presses,
                     (unsigned long long)s.key_state_release_without_press, (unsigned long long)s.keyboard_enter_events,
                     (unsigned long long)s.pointer_events_rx, (unsigned long long)s.pointer_events_injected,
                     (unsigned long long)s.pointer_events_dropped, (unsigned long long)s.pointer_button_grab_started,
                     (unsigned long long)s.pointer_button_grab_ended, (unsigned long long)s.pointer_button_grab_cleared,
                     (unsigned long long)s.pointer_button_grab_surface_destroyed,
                     wd_avg_ms(s.input_queue_latency_sum_ns, s.input_queue_latency_samples),
                     wd_avg_ms(s.input_to_summary_sum_ns, s.input_to_summary_samples),
                     wd_avg_ms(s.input_to_first_fresh_tile_sum_ns, s.input_to_first_fresh_tile_samples));
    }

    bool compositor_activity = s.xdg_move_invalid_serial != 0 || s.xdg_resize_invalid_serial != 0 ||
                               s.popup_explicit_scene_trees != 0 || s.popup_explicit_scene_tree_failures != 0 ||
                               s.cursor_shape_requests != 0 || s.cursor_shape_tx != 0 ||
                               s.cursor_shape_coalesced != 0 || s.cursor_set_cursor_requests != 0 ||
                               s.cursor_set_cursor_rejected != 0 || s.cursor_set_cursor_hidden != 0 ||
                               s.cursor_set_cursor_fallback != 0;
    if (compositor_activity)
    {
        WD_LOG_DEBUG("compositor/min: xdg_move_bad_serial=%llu xdg_resize_bad_serial=%llu popup_scene=%llu popup_scene_fail=%llu cursor_shape=%llu cursor_shape_tx=%llu cursor_shape_coalesced=%llu cursor_set=%llu cursor_reject=%llu cursor_hidden=%llu cursor_fallback=%llu",
                     (unsigned long long)s.xdg_move_invalid_serial, (unsigned long long)s.xdg_resize_invalid_serial,
                     (unsigned long long)s.popup_explicit_scene_trees, (unsigned long long)s.popup_explicit_scene_tree_failures,
                     (unsigned long long)s.cursor_shape_requests, (unsigned long long)s.cursor_shape_tx,
                     (unsigned long long)s.cursor_shape_coalesced, (unsigned long long)s.cursor_set_cursor_requests,
                     (unsigned long long)s.cursor_set_cursor_rejected, (unsigned long long)s.cursor_set_cursor_hidden,
                     (unsigned long long)s.cursor_set_cursor_fallback);
    }
}
