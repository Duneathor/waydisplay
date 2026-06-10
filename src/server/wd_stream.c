#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"
#include "wd_server.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define WD_NSEC_PER_SEC 1000000000ull

#define WD_UDP_SEND_PRESSURE_LOG_INTERVAL_NS 1000000000ull
#define WD_ENCODER_MAX_THREADS 4u
#define WD_ENCODER_BATCH_JOBS 256u

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


static bool wd_stream_client_packet_loss_sample(const struct wd_stats* stats) {
    if (!stats || stats->client_stats_rx == 0 || stats->client_udp_packets_rx < WD_STREAM_CLIENT_COMPLETION_MIN_PACKETS)
    {
        return false;
    }

    return stats->client_completed_packets * 100ull <
           stats->client_udp_packets_rx * (uint64_t)WD_STREAM_CLIENT_COMPLETION_LOSS_PERCENT;
}

static bool wd_stream_client_reporting_tile_loss_locked(const struct wd_stream_policy* policy, const struct wd_stats* stats) {
    if (stats && (stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0 || wd_stream_client_packet_loss_sample(stats)))
    {
        return true;
    }

    return policy && policy->multipacket_loss_cooldown_seconds != 0;
}

static void wd_stream_policy_update_frame_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool link_loss,
                                                       bool send_pressure, bool client_loss) {
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

    if (link_loss)
    {
        policy->frame_rate_good_seconds = 0;

        uint32_t decrease_percent = send_pressure ? WD_STREAM_FPS_PRESSURE_DECREASE_PERCENT : WD_STREAM_FPS_DECREASE_PERCENT;
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
            WD_LOG_INFO("WayDisplay: stream frame rate down: %u -> %u fps%s", old_fps, (unsigned)new_fps,
                        send_pressure ? " due to UDP send pressure" :
                        (client_loss ? " due to client loss" : " due to repair pressure"));
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
        WD_LOG_INFO("WayDisplay: stream frame rate up: %u -> %u fps", old_fps, (unsigned)new_fps);
    }
}

static void wd_stream_policy_update_limited_rate_locked(struct wd_stream_policy* policy, struct wd_stats* stats, bool link_loss,
                                                        bool send_pressure, bool tile_loss) {
    if (!policy || !stats)
    {
        return;
    }

    const bool useful_tile_activity = stats->udp_tiles_sent != 0 || stats->dirty_tiles != 0 || stats->client_tiles_completed != 0;
    uint64_t old_rate = wd_stream_clamp_limited_udp_byte_rate(policy->limited_udp_bytes_per_second);
    uint64_t new_rate = old_rate;

    if (link_loss)
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

        uint32_t decrease_percent = send_pressure ? WD_STREAM_RATE_PRESSURE_DECREASE_PERCENT : WD_STREAM_RATE_DECREASE_PERCENT;
        new_rate = old_rate * (uint64_t)decrease_percent / 100ull;

        wd_stream_policy_set_limited_rate_locked(policy, new_rate);
        if (policy->limited_udp_bytes_per_second != old_rate)
        {
            stats->rate_decreases++;
            WD_LOG_INFO("WayDisplay: stream byte budget down: %llu -> %llu KiB/s%s",
                        (unsigned long long)(old_rate / 1024ull),
                        (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull),
                        send_pressure ? " due to UDP send pressure" :
                        (tile_loss ? " due to client tile loss" : " due to low client packet completion"));
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
        WD_LOG_INFO("WayDisplay: stream byte budget up: %llu -> %llu KiB/s",
                    (unsigned long long)(old_rate / 1024ull),
                    (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
    }
}

static void wd_stream_policy_update_health_locked(struct wd_stream_policy* policy, struct wd_stats* stats) {
    if (!policy || !stats)
    {
        return;
    }

    const bool send_pressure = stats->udp_send_pressure_drops != 0;
    const bool client_packet_loss = wd_stream_client_packet_loss_sample(stats);
    const bool client_tile_loss = stats->client_partial_tiles_timed_out != 0 || stats->client_retx_requests_tx != 0;
    const bool link_loss = send_pressure || client_packet_loss || client_tile_loss;

    if (client_tile_loss || client_packet_loss)
    {
        policy->multipacket_loss_cooldown_seconds = WD_STREAM_MULTIPACKET_LOSS_COOLDOWN_SECONDS;
    }
    else if (policy->multipacket_loss_cooldown_seconds != 0)
    {
        policy->multipacket_loss_cooldown_seconds--;
    }

    /* Tile-size selection is intentionally not a stateful policy anymore.
     * The sender chooses per dirty tile from the actual compressed wire size.
     * This update only controls how quickly the byte token bucket refills and
     * how often new frames are rendered under clear link pressure. */
    wd_stream_policy_update_frame_rate_locked(policy, stats, link_loss, send_pressure, client_packet_loss || client_tile_loss);
    wd_stream_policy_update_limited_rate_locked(policy, stats, link_loss, send_pressure, client_tile_loss);
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

    if (!client_connected)
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

    uint16_t fps = wd_stream_policy_effective_fps_locked(policy);
    uint64_t interval_ns = 1000000000ull / fps;

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
    if (!server || !server->net.tiles)
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

    if (!server->net.dirty_regions || !server->net.dirty_region_queued || !server->net.dirty_epochs || !server->net.dirty_queue ||
        !server->net.dirty_queued || !server->net.dirty_queue_enqueued_ns ||
        !server->net.retransmit_queue || !server->net.retransmit_queued || !server->net.retransmit_queue_enqueued_ns ||
        !server->net.retransmit_requested_generation || !server->net.summary_dirty_tiles)
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
    if (net->udp_send_pressure_log_ns != 0 && now - net->udp_send_pressure_log_ns < WD_UDP_SEND_PRESSURE_LOG_INTERVAL_NS)
    {
        return;
    }

    uint64_t drops = net->udp_send_pressure_drops;
    net->udp_send_pressure_drops = 0;
    net->udp_send_pressure_log_ns = now;

    WD_LOG_DEBUG("WayDisplay: dropped %llu UDP tile packets under send pressure: %s", (unsigned long long)drops, strerror(send_errno));
}


static void wd_stream_mark_summary_dirty_locked(struct wd_server* server, uint16_t tile_id) {
    if (!server || tile_id >= server->total_tiles)
    {
        return;
    }

    struct wd_net_state* net = &server->net;

    if (!net->summary_dirty_tiles)
    {
        return;
    }

    if (!net->summary_dirty_tiles[tile_id])
    {
        net->summary_dirty_tiles[tile_id] = true;
        net->summary_dirty_count++;
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
                                              uint16_t udp_payload_target, uint64_t input_sequence, bool compressed_payload) {
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

static void wd_stream_policy_consume_limited_bytes_locked(struct wd_stream_policy* policy, uint32_t bytes) {
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

            WD_LOG_ERROR("WayDisplay: sendto failed: %s", strerror(errno));
            return false;
        }

        if (result)
        {
            result->any_packet_sent = true;
            result->packets_sent++;
            result->bytes_sent += (uint32_t)sent;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)sent;
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

static bool wd_stream_send_current_base_tile_locked(struct wd_server* server, uint16_t tile_id, uint64_t input_sequence,
                                                     uint8_t* tile_bytes, uint8_t* compressed_tile, size_t compressed_capacity,
                                                     struct wd_udp_tile_send_result* result) {
    if (!server || !tile_bytes || !compressed_tile || tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    struct wd_tile_state* tile = &net->tiles[tile_id];
    const uint64_t timestamp_ns = wd_now_ns();

    uint64_t encode_start_ns = wd_now_ns();
    if (!wd_extract_tile_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                           server->tiles_x, server->total_tiles, tile_id, server->tile_width,
                                           server->tile_height, tile_bytes))
    {
        return false;
    }

    uint32_t compressed_size = 0;
    if (!wd_zstd_compress(tile_bytes, server->uncompressed_tile_bytes, compressed_tile, (uint32_t)compressed_capacity,
                          WD_ZSTD_LEVEL, &compressed_size))
    {
        return false;
    }
    net->stats.tile_encode_ns += wd_now_ns() - encode_start_ns;

    const bool compressed_payload = wd_stream_use_compressed_tile_payload(compressed_size, server->uncompressed_tile_bytes,
                                                                         net->udp_payload_target, input_sequence);
    const uint8_t* payload = compressed_payload ? compressed_tile : tile_bytes;
    const uint32_t payload_size = compressed_payload ? compressed_size : server->uncompressed_tile_bytes;

    return wd_stream_send_tile_payload_sized_locked(server, tile_id, server->tile_width, server->tile_height, tile->generation,
                                                    timestamp_ns, input_sequence, payload, payload_size, compressed_payload, result);
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

static void wd_tile_queue_remove_at_locked(uint16_t* queue, uint16_t* queue_count, uint16_t index) {
    if (!queue || !queue_count || index >= *queue_count)
    {
        return;
    }

    uint16_t tail_count = (uint16_t)(*queue_count - index - 1u);
    if (tail_count > 0)
    {
        memmove(&queue[index], &queue[index + 1u], (size_t)tail_count * sizeof(queue[0]));
    }

    (*queue_count)--;
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

static bool wd_dirty_queue_reinsert_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    return wd_dirty_queue_push_locked(net, tile_id, total_tiles);
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
};

struct wd_parallel_encode_job {
    struct wd_server* server;
    uint16_t top_region_id;
    uint64_t input_sequence;
    uint64_t remaining_byte_budget;
    bool network_happy;
    const bool* dirty_snapshot;
    const uint64_t* dirty_epoch_snapshot;
    struct wd_parallel_encode_result* result;
};

struct wd_parallel_encode_batch {
    struct wd_parallel_encode_job* jobs;
    uint16_t job_count;
    uint16_t next_job;
    pthread_mutex_t lock;
};

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

static bool wd_stream_wire_tile_region_has_dirty_locked(struct wd_server* server, uint16_t wire_tile_id, uint16_t tile_width,
                                                        uint16_t tile_height, uint16_t* out_ids, uint16_t* out_count,
                                                        uint16_t max_count) {
    if (!server || !out_ids || !out_count || !server->net.dirty_queued)
    {
        return false;
    }

    if (!wd_stream_collect_wire_tile_base_ids(server, wire_tile_id, tile_width, tile_height, out_ids, out_count, max_count))
    {
        return false;
    }

    bool dirty = false;
    for (uint16_t i = 0; i < *out_count; ++i)
    {
        const uint16_t base_id = out_ids[i];
        if (base_id >= server->total_tiles || !server->net.dirty_queued[base_id])
        {
            continue;
        }

        dirty = true;
    }

    return dirty;
}

static bool wd_stream_try_build_wire_tile_candidate_for_region(struct wd_server* server, uint16_t wire_tile_id, uint16_t tile_width,
                                                               uint16_t tile_height, uint64_t input_sequence, bool allow_compression,
                                                               uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                               size_t compressed_capacity, struct wd_wire_tile_candidate* out) {
    if (!server || !tile_bytes || !compressed_tile || !out)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_collect_wire_tile_base_ids(server, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                             (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const uint32_t uncompressed_size = (uint32_t)tile_width * (uint32_t)tile_height * WD_BYTES_PER_PIXEL;
    const uint64_t encode_start_ns = wd_now_ns();
    const uint16_t wire_tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t wire_total_tiles = wd_total_tiles_for_size_with_tile(server->display_width, server->display_height, tile_width, tile_height);
    if (!wd_extract_tile_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
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
        compressed_payload = wd_stream_use_compressed_tile_payload(compressed_size, uncompressed_size, server->net.udp_payload_target,
                                                                   input_sequence);
    }

    const uint32_t payload_size = compressed_payload ? compressed_size : uncompressed_size;
    server->net.stats.tile_encode_ns += wd_now_ns() - encode_start_ns;

    memset(out, 0, sizeof(*out));
    out->width = tile_width;
    out->height = tile_height;
    out->tile_id = wire_tile_id;
    memcpy(out->covered_base_ids, covered_ids, (size_t)covered_count * sizeof(covered_ids[0]));
    out->covered_base_count = covered_count;
    out->uncompressed_size = uncompressed_size;
    out->compressed_size = compressed_size;
    out->wire_size = wd_stream_tile_wire_bytes_for_payload(payload_size, server->net.udp_payload_target, input_sequence, compressed_payload);
    out->compressed_payload = compressed_payload;
    return true;
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

static bool wd_stream_choose_region_recursive_locked(struct wd_server* server, uint16_t wire_tile_id, uint16_t tile_width,
                                                     uint16_t tile_height, uint64_t input_sequence, uint64_t remaining_byte_budget,
                                                     bool network_happy, uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                     size_t compressed_capacity, struct wd_wire_tile_candidate* out,
                                                     bool* out_budget_blocked) {
    if (out_budget_blocked)
    {
        *out_budget_blocked = false;
    }
    if (!server || !out)
    {
        return false;
    }

    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    server->net.stats.dirty_region_probes++;
    if (!wd_stream_wire_tile_region_has_dirty_locked(server, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                                     (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }
    server->net.stats.dirty_region_hits++;

    const bool is_base_tile = tile_width == server->tile_width && tile_height == server->tile_height;
    const bool allow_compression = !is_base_tile;
    struct wd_wire_tile_candidate candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (wd_stream_try_build_wire_tile_candidate_for_region(server, wire_tile_id, tile_width, tile_height, input_sequence,
                                                           allow_compression, tile_bytes, compressed_tile, compressed_capacity,
                                                           &candidate) &&
        wd_stream_candidate_allowed_for_region_locked(server, &candidate, remaining_byte_budget, network_happy))
    {
        *out = candidate;
        return true;
    }

    if (is_base_tile)
    {
        if (candidate.wire_size > remaining_byte_budget && out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
        return false;
    }

    const uint16_t parent_tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
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
            if (xs[i] < server->display_width && start_y < server->display_height &&
                wd_stream_wire_tile_for_pixel(server, xs[i], start_y, child_width, child_height, &child_ids[child_count]))
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
                if (xs[x] < server->display_width && ys[y] < server->display_height &&
                    wd_stream_wire_tile_for_pixel(server, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
                {
                    child_count++;
                }
            }
        }
    }
    else if (tile_width == 32 && tile_height == 32)
    {
        child_width = server->tile_width;
        child_height = server->tile_height;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        const uint32_t ys[2] = {start_y, start_y + child_height};
        for (uint16_t y = 0; y < 2; ++y)
        {
            for (uint16_t x = 0; x < 2; ++x)
            {
                if (xs[x] < server->display_width && ys[y] < server->display_height &&
                    wd_stream_wire_tile_for_pixel(server, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
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

    if (child_count == 0)
    {
        return false;
    }

    uint32_t r = server->net.dirty_region_rng = server->net.dirty_region_rng * 1664525u + 1013904223u;
    uint16_t first = (uint16_t)(r % child_count);
    for (uint16_t i = 0; i < child_count; ++i)
    {
        uint16_t child_index = (uint16_t)((first + i) % child_count);
        bool child_budget_blocked = false;
        if (wd_stream_choose_region_recursive_locked(server, child_ids[child_index], child_width, child_height, input_sequence,
                                                     remaining_byte_budget, network_happy, tile_bytes, compressed_tile,
                                                     compressed_capacity, out, &child_budget_blocked))
        {
            return true;
        }
        if (child_budget_blocked && out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
    }

    return false;
}


static bool wd_stream_snapshot_region_has_dirty(const struct wd_server* server, const bool* dirty_snapshot,
                                                uint16_t wire_tile_id, uint16_t tile_width, uint16_t tile_height,
                                                uint16_t* out_ids, uint16_t* out_count, uint16_t max_count) {
    if (!server || !dirty_snapshot || !out_ids || !out_count)
    {
        return false;
    }
    if (!wd_stream_collect_wire_tile_base_ids(server, wire_tile_id, tile_width, tile_height, out_ids, out_count, max_count))
    {
        return false;
    }
    for (uint16_t i = 0; i < *out_count; ++i)
    {
        const uint16_t base_id = out_ids[i];
        if (base_id < server->total_tiles && dirty_snapshot[base_id])
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
    if (!job || !job->server || !tile_bytes || !compressed_tile || !out || !out_epochs)
    {
        return false;
    }

    struct wd_server* server = job->server;
    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_collect_wire_tile_base_ids(server, wire_tile_id, tile_width, tile_height, covered_ids, &covered_count,
                                             (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const uint32_t uncompressed_size = (uint32_t)tile_width * (uint32_t)tile_height * WD_BYTES_PER_PIXEL;
    const uint16_t wire_tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
    const uint16_t wire_total_tiles = wd_total_tiles_for_size_with_tile(server->display_width, server->display_height, tile_width, tile_height);
    if (!wd_extract_tile_xrgb8888_for_tile(server->framebuffer_xrgb8888, server->display_width, server->display_height,
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
                                                                   server->net.udp_payload_target, job->input_sequence);
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
    out->wire_size = wd_stream_tile_wire_bytes_for_payload(payload_size, server->net.udp_payload_target,
                                                           job->input_sequence, compressed_payload);
    out->compressed_payload = compressed_payload;
    for (uint16_t i = 0; i < covered_count; ++i)
    {
        out_epochs[i] = job->dirty_epoch_snapshot ? job->dirty_epoch_snapshot[covered_ids[i]] : 0;
    }
    return true;
}

static bool wd_stream_choose_region_recursive_snapshot(struct wd_parallel_encode_job* job, uint16_t wire_tile_id,
                                                       uint16_t tile_width, uint16_t tile_height,
                                                       uint8_t* tile_bytes, uint8_t* compressed_tile,
                                                       size_t compressed_capacity, struct wd_wire_tile_candidate* out,
                                                       uint64_t* out_epochs, bool* out_budget_blocked) {
    if (out_budget_blocked)
    {
        *out_budget_blocked = false;
    }
    if (!job || !job->server || !out || !out_epochs)
    {
        return false;
    }

    struct wd_server* server = job->server;
    uint16_t covered_count = 0;
    uint16_t covered_ids[64];
    if (!wd_stream_snapshot_region_has_dirty(server, job->dirty_snapshot, wire_tile_id, tile_width, tile_height,
                                             covered_ids, &covered_count, (uint16_t)(sizeof(covered_ids) / sizeof(covered_ids[0]))))
    {
        return false;
    }

    const bool is_base_tile = tile_width == server->tile_width && tile_height == server->tile_height;
    const bool allow_compression = !is_base_tile;
    struct wd_wire_tile_candidate candidate;
    uint64_t candidate_epochs[64] = {0};
    memset(&candidate, 0, sizeof(candidate));
    if (wd_stream_try_encode_candidate_for_snapshot(job, wire_tile_id, tile_width, tile_height, allow_compression,
                                                    tile_bytes, compressed_tile, compressed_capacity, &candidate,
                                                    candidate_epochs) &&
        wd_stream_candidate_allowed_for_region_locked(server, &candidate, job->remaining_byte_budget, job->network_happy))
    {
        *out = candidate;
        memcpy(out_epochs, candidate_epochs, (size_t)candidate.covered_base_count * sizeof(candidate_epochs[0]));
        return true;
    }

    if (is_base_tile)
    {
        if (candidate.wire_size > job->remaining_byte_budget && out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
        return false;
    }

    const uint16_t parent_tiles_x = wd_tiles_for_width_with_tile(server->display_width, tile_width);
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
            if (xs[i] < server->display_width && start_y < server->display_height &&
                wd_stream_wire_tile_for_pixel(server, xs[i], start_y, child_width, child_height, &child_ids[child_count]))
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
                if (xs[x] < server->display_width && ys[y] < server->display_height &&
                    wd_stream_wire_tile_for_pixel(server, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
                {
                    child_count++;
                }
            }
        }
    }
    else if (tile_width == 32 && tile_height == 32)
    {
        child_width = server->tile_width;
        child_height = server->tile_height;
        const uint32_t xs[2] = {start_x, start_x + child_width};
        const uint32_t ys[2] = {start_y, start_y + child_height};
        for (uint16_t y = 0; y < 2; ++y)
        {
            for (uint16_t x = 0; x < 2; ++x)
            {
                if (xs[x] < server->display_width && ys[y] < server->display_height &&
                    wd_stream_wire_tile_for_pixel(server, xs[x], ys[y], child_width, child_height, &child_ids[child_count]))
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

    for (uint16_t i = 0; i < child_count; ++i)
    {
        bool child_budget_blocked = false;
        if (wd_stream_choose_region_recursive_snapshot(job, child_ids[i], child_width, child_height, tile_bytes,
                                                       compressed_tile, compressed_capacity, out, out_epochs,
                                                       &child_budget_blocked))
        {
            return true;
        }
        if (child_budget_blocked && out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
    }
    return false;
}

static void wd_stream_parallel_encode_one_job(struct wd_parallel_encode_job* job) {
    if (!job || !job->result || !job->server)
    {
        return;
    }
    struct wd_parallel_encode_result* result = job->result;
    memset(result, 0, sizeof(*result));
    result->top_region_id = job->top_region_id;

    const uint32_t max_wire_tile_bytes = WD_WIRE_TILE_MAX_WIDTH * WD_WIRE_TILE_MAX_HEIGHT * WD_BYTES_PER_PIXEL;
    uint8_t* tile_bytes = malloc(max_wire_tile_bytes);
    size_t compressed_capacity = wd_zstd_compress_bound(max_wire_tile_bytes);
    uint8_t* compressed_tile = malloc(compressed_capacity);
    if (!tile_bytes || !compressed_tile)
    {
        free(compressed_tile);
        free(tile_bytes);
        return;
    }

    const uint64_t start_ns = wd_now_ns();
    bool budget_blocked = false;
    struct wd_wire_tile_candidate candidate;
    uint64_t covered_epochs[64] = {0};
    memset(&candidate, 0, sizeof(candidate));
    if (wd_stream_choose_region_recursive_snapshot(job, job->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                   WD_WIRE_TILE_MAX_HEIGHT, tile_bytes, compressed_tile,
                                                   compressed_capacity, &candidate, covered_epochs, &budget_blocked))
    {
        const uint8_t* payload = candidate.compressed_payload ? compressed_tile : tile_bytes;
        const uint32_t payload_size = candidate.compressed_payload ? candidate.compressed_size : candidate.uncompressed_size;
        result->payload = malloc(payload_size);
        if (result->payload)
        {
            memcpy(result->payload, payload, payload_size);
            result->payload_size = payload_size;
            result->candidate = candidate;
            memcpy(result->covered_dirty_epochs, covered_epochs,
                   (size_t)candidate.covered_base_count * sizeof(covered_epochs[0]));
            result->valid = true;
        }
    }
    result->budget_blocked = budget_blocked;
    result->worker_encode_ns = wd_now_ns() - start_ns;
    free(compressed_tile);
    free(tile_bytes);
}

static void* wd_stream_parallel_encode_worker(void* data) {
    struct wd_parallel_encode_batch* batch = data;
    if (!batch)
    {
        return NULL;
    }
    for (;;)
    {
        pthread_mutex_lock(&batch->lock);
        uint16_t index = batch->next_job;
        if (index < batch->job_count)
        {
            batch->next_job++;
        }
        pthread_mutex_unlock(&batch->lock);
        if (index >= batch->job_count)
        {
            break;
        }
        wd_stream_parallel_encode_one_job(&batch->jobs[index]);
    }
    return NULL;
}

static bool wd_stream_choose_random_dirty_region_locked(struct wd_server* server, uint64_t input_sequence,
                                                        uint64_t remaining_byte_budget, uint8_t* tile_bytes,
                                                        uint8_t* compressed_tile, size_t compressed_capacity,
                                                        struct wd_wire_tile_candidate* out, bool* out_budget_blocked) {
    if (out_budget_blocked)
    {
        *out_budget_blocked = false;
    }
    if (!server || !out || server->total_tiles == 0 || server->net.dirty_region_count == 0)
    {
        return false;
    }

    const uint32_t top_uncompressed_bytes = WD_WIRE_TILE_MAX_WIDTH * WD_WIRE_TILE_MAX_HEIGHT * WD_BYTES_PER_PIXEL;
    const uint32_t minimum_wire_bytes = wd_stream_tile_wire_bytes_for_payload(top_uncompressed_bytes,
                                                                              server->net.udp_payload_target, input_sequence, false);
    if (minimum_wire_bytes == 0 || minimum_wire_bytes > remaining_byte_budget)
    {
        if (out_budget_blocked)
        {
            *out_budget_blocked = true;
        }
        return false;
    }

    const bool network_happy = !wd_stream_client_reporting_tile_loss_locked(&server->net.stream_policy, &server->net.stats);
    if (server->net.dirty_region_rng == 0)
    {
        server->net.dirty_region_rng = (uint32_t)(wd_now_ns() ^ ((uint64_t)server->display_width << 16) ^ server->display_height);
        if (server->net.dirty_region_rng == 0)
        {
            server->net.dirty_region_rng = 1;
        }
    }

    bool budget_blocked = false;
    const uint16_t attempts = server->net.dirty_region_count;
    for (uint16_t i = 0; i < attempts && server->net.dirty_region_count > 0; ++i)
    {
        const uint64_t select_start_ns = wd_now_ns();
        uint32_t r = server->net.dirty_region_rng = server->net.dirty_region_rng * 1664525u + 1013904223u;
        uint16_t index = (uint16_t)(r % server->net.dirty_region_count);
        uint16_t top_id = server->net.dirty_regions[index];

        if (!wd_stream_top_region_still_dirty_locked(server, top_id))
        {
            wd_stream_remove_dirty_top_region_locked(server, top_id);
            server->net.stats.dirty_region_select_ns += wd_now_ns() - select_start_ns;
            continue;
        }
        server->net.stats.dirty_region_select_ns += wd_now_ns() - select_start_ns;

        bool candidate_budget_blocked = false;
        if (wd_stream_choose_region_recursive_locked(server, top_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT,
                                                     input_sequence, remaining_byte_budget, network_happy, tile_bytes,
                                                     compressed_tile, compressed_capacity, out, &candidate_budget_blocked))
        {
            return true;
        }
        if (!wd_stream_top_region_still_dirty_locked(server, top_id))
        {
            wd_stream_remove_dirty_top_region_locked(server, top_id);
        }
        if (candidate_budget_blocked)
        {
            budget_blocked = true;
            break;
        }
    }

    if (out_budget_blocked)
    {
        *out_budget_blocked = budget_blocked;
    }
    return false;
}

static bool wd_retransmit_queue_pop_sendable_locked(struct wd_server* server, uint64_t remaining_byte_budget,
                                                    uint16_t* out_tile_id, uint64_t* out_requested_generation,
                                                    uint32_t* out_predicted_bytes, bool* out_budget_blocked) {
    if (out_budget_blocked)
    {
        *out_budget_blocked = false;
    }
    if (!server || !out_tile_id || !out_requested_generation || !out_predicted_bytes)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    while (net->retransmit_queue_count > 0)
    {
        uint16_t tile_id = net->retransmit_queue[0];
        wd_tile_queue_remove_at_locked(net->retransmit_queue, &net->retransmit_queue_count, 0);

        if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
        {
            if (tile_id < server->total_tiles && net->retransmit_queue_enqueued_ns)
            {
                net->retransmit_queue_enqueued_ns[tile_id] = 0;
            }
            if (tile_id < server->total_tiles && net->retransmit_requested_generation)
            {
                net->retransmit_requested_generation[tile_id] = 0;
            }
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

        uint64_t requested_generation = net->retransmit_requested_generation ? net->retransmit_requested_generation[tile_id] : 0;
        struct wd_tile_state* tile = &net->tiles[tile_id];
        if (requested_generation != 0 && tile->generation < requested_generation)
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
            net->stats.retx_req_waiting_for_generation++;
            continue;
        }

        uint64_t retx_input_sequence = net->input_since_last_fresh_tile ? net->last_input_sequence : tile->input_sequence;
        uint32_t predicted_bytes = wd_stream_tile_wire_bytes_for_payload(server->uncompressed_tile_bytes, net->udp_payload_target,
                                                                         retx_input_sequence, false);
        if (predicted_bytes == 0 || predicted_bytes > remaining_byte_budget)
        {
            wd_stream_queue_retransmit_tile_locked(server, tile_id, requested_generation);
            if (out_budget_blocked)
            {
                *out_budget_blocked = true;
            }
            return false;
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

        *out_tile_id = tile_id;
        *out_requested_generation = requested_generation;
        *out_predicted_bytes = predicted_bytes;
        return true;
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

static void wd_stream_send_retransmits_locked(struct wd_server* server, uint64_t now, uint64_t max_bytes,
                                                uint8_t* tile_bytes, uint8_t* compressed_tile, size_t compressed_capacity) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    uint64_t spent_bytes = 0;

    while (net->retransmit_queue_count > 0)
    {
        uint64_t token_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
        if (max_bytes != 0)
        {
            if (spent_bytes >= max_bytes)
            {
                break;
            }
            uint64_t remaining_slice = max_bytes - spent_bytes;
            if (token_budget > remaining_slice)
            {
                token_budget = remaining_slice;
            }
        }

        uint16_t tile_id = 0;
        uint64_t requested_generation = 0;
        uint32_t predicted_bytes = 0;
        bool budget_blocked = false;
        if (!wd_retransmit_queue_pop_sendable_locked(server, token_budget, &tile_id, &requested_generation, &predicted_bytes,
                                                     &budget_blocked))
        {
            (void)budget_blocked;
            break;
        }

        struct wd_udp_tile_send_result send_result;
        uint64_t retx_input_sequence = net->input_since_last_fresh_tile ? net->last_input_sequence : 0;
        if (!wd_stream_send_current_base_tile_locked(server, tile_id, retx_input_sequence, tile_bytes, compressed_tile, compressed_capacity, &send_result))
        {
            continue;
        }

        if (!send_result.all_packets_sent)
        {
            if (send_result.any_packet_sent)
            {
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                spent_bytes += send_result.bytes_sent;
            }
            wd_stream_queue_retransmit_tile_locked(server, tile_id, requested_generation);
            break;
        }

        net->stats.udp_retx_tiles_sent++;
        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
        spent_bytes += send_result.bytes_sent;

        if (send_result.send_blocked)
        {
            break;
        }

        (void)predicted_bytes;
    }
}

bool wd_stream_send_dirty_tiles(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    if (!server->framebuffer_xrgb8888)
    {
        return false;
    }

    const uint64_t now = wd_now_ns();

    const uint32_t max_wire_tile_bytes = WD_WIRE_TILE_MAX_WIDTH * WD_WIRE_TILE_MAX_HEIGHT * WD_BYTES_PER_PIXEL;
    uint8_t* tile_bytes = malloc(max_wire_tile_bytes);
    if (!tile_bytes)
    {
        return false;
    }

    size_t   compressed_capacity = wd_zstd_compress_bound(max_wire_tile_bytes);
    uint8_t* compressed_tile     = malloc(compressed_capacity);

    if (!compressed_tile)
    {
        free(tile_bytes);
        return false;
    }

    pthread_mutex_lock(&net->lock);

    if (!net->client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        free(compressed_tile);
        free(tile_bytes);
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
        uint64_t initial_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
        uint64_t repair_budget = initial_budget * (uint64_t)WD_RETRANSMIT_LOSS_BUDGET_PERCENT / 100ull;
        if (repair_budget == 0 && initial_budget != 0)
        {
            repair_budget = initial_budget;
        }
        wd_stream_send_retransmits_locked(server, now, repair_budget, tile_bytes, compressed_tile, compressed_capacity);
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
        struct wd_parallel_encode_result* results = calloc(batch_capacity, sizeof(*results));
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

            jobs[job_count].server = server;
            jobs[job_count].top_region_id = top_id;
            jobs[job_count].input_sequence = tile_input_sequence;
            jobs[job_count].remaining_byte_budget = remaining_byte_budget;
            jobs[job_count].network_happy = network_happy;
            jobs[job_count].dirty_snapshot = dirty_snapshot;
            jobs[job_count].dirty_epoch_snapshot = epoch_snapshot;
            jobs[job_count].result = &results[job_count];
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
        pthread_mutex_init(&batch.lock, NULL);

        uint16_t thread_count = wd_stream_encoder_thread_count();
        if (thread_count > job_count)
        {
            thread_count = job_count;
        }
        if (thread_count == 0)
        {
            thread_count = 1;
        }

        pthread_t threads[WD_ENCODER_MAX_THREADS];
        memset(threads, 0, sizeof(threads));
        uint16_t created_threads = 0;
        const uint64_t wait_start_ns = wd_now_ns();
        pthread_mutex_unlock(&net->lock);
        for (uint16_t i = 0; i < thread_count; ++i)
        {
            if (pthread_create(&threads[i], NULL, wd_stream_parallel_encode_worker, &batch) == 0)
            {
                created_threads++;
            }
            else
            {
                threads[i] = 0;
            }
        }
        if (created_threads == 0)
        {
            wd_stream_parallel_encode_worker(&batch);
            created_threads = 1;
        }
        for (uint16_t i = 0; i < thread_count; ++i)
        {
            if (threads[i])
            {
                pthread_join(threads[i], NULL);
            }
        }
        pthread_mutex_lock(&net->lock);
        net->stats.encode_wait_ns += wd_now_ns() - wait_start_ns;
        net->stats.encode_threads_used += created_threads;
        pthread_mutex_destroy(&batch.lock);

        bool stop_sending = false;
        for (uint16_t ri = 0; ri < job_count; ++ri)
        {
            struct wd_parallel_encode_result* result = &results[ri];
            net->stats.encode_worker_ns += result->worker_encode_ns;
            net->stats.tile_encode_ns += result->worker_encode_ns;
            if (!result->valid)
            {
                if (result->budget_blocked && net->dirty_region_count > 0)
                {
                    net->stats.dirty_budget_blocked++;
                    stop_sending = true;
                }
                if (wd_stream_top_region_still_dirty_locked(server, result->top_region_id))
                {
                    uint16_t first_base = 0;
                    uint16_t count = 0;
                    uint16_t ids[64];
                    if (wd_stream_collect_wire_tile_base_ids(server, result->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[first_base]);
                    }
                }
                free(result->payload);
                result->payload = NULL;
                if (stop_sending)
                {
                    break;
                }
                continue;
            }

            bool stale = false;
            for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
            {
                const uint16_t covered_id = result->candidate.covered_base_ids[i];
                if (covered_id >= server->total_tiles)
                {
                    stale = true;
                    break;
                }
                if (net->dirty_epochs && net->dirty_epochs[covered_id] != result->covered_dirty_epochs[i])
                {
                    stale = true;
                    break;
                }
            }
            if (stale)
            {
                net->stats.encode_jobs_stale++;
                if (wd_stream_top_region_still_dirty_locked(server, result->top_region_id))
                {
                    uint16_t ids[64];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, result->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
                free(result->payload);
                result->payload = NULL;
                continue;
            }

            const uint64_t current_budget = wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now);
            const bool current_network_happy = !wd_stream_client_reporting_tile_loss_locked(&net->stream_policy, &net->stats);
            if (!wd_stream_candidate_allowed_for_region_locked(server, &result->candidate, current_budget, current_network_happy))
            {
                net->stats.dirty_budget_blocked++;
                if (wd_stream_top_region_still_dirty_locked(server, result->top_region_id))
                {
                    uint16_t ids[64];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, result->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
                free(result->payload);
                result->payload = NULL;
                stop_sending = true;
                break;
            }

            uint64_t next_generation = 1;
            for (uint16_t i = 0; i < result->candidate.covered_base_count; ++i)
            {
                const uint16_t covered_id = result->candidate.covered_base_ids[i];
                if (covered_id < server->total_tiles && net->tiles[covered_id].generation >= next_generation)
                {
                    next_generation = net->tiles[covered_id].generation + 1;
                }
            }

            struct wd_udp_tile_send_result send_result;
            if (!wd_stream_send_tile_payload_sized_locked(server, result->candidate.tile_id, result->candidate.width,
                                                          result->candidate.height, next_generation, now, tile_input_sequence,
                                                          result->payload, result->payload_size,
                                                          result->candidate.compressed_payload, &send_result))
            {
                if (wd_stream_top_region_still_dirty_locked(server, result->top_region_id))
                {
                    uint16_t ids[64];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, result->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
                free(result->payload);
                result->payload = NULL;
                continue;
            }

            if (!send_result.all_packets_sent)
            {
                if (send_result.any_packet_sent)
                {
                    wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, send_result.bytes_sent);
                }
                if (wd_stream_top_region_still_dirty_locked(server, result->top_region_id))
                {
                    uint16_t ids[64];
                    uint16_t count = 0;
                    if (wd_stream_collect_wire_tile_base_ids(server, result->top_region_id, WD_WIRE_TILE_MAX_WIDTH,
                                                             WD_WIRE_TILE_MAX_HEIGHT, ids, &count,
                                                             (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                    {
                        wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                    }
                }
                free(result->payload);
                result->payload = NULL;
                stop_sending = true;
                break;
            }

            wd_stream_note_tile_choice_locked(net, result->candidate.compressed_size, result->candidate.uncompressed_size,
                                              net->udp_payload_target, tile_input_sequence,
                                              result->candidate.compressed_payload);

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
            free(result->payload);
            result->payload = NULL;

            if (send_result.send_blocked)
            {
                stop_sending = true;
                break;
            }
        }

        for (uint16_t ri = 0; ri < job_count; ++ri)
        {
            const uint16_t top_id = jobs[ri].top_region_id;
            if (wd_stream_top_region_still_dirty_locked(server, top_id))
            {
                uint16_t ids[64];
                uint16_t count = 0;
                if (wd_stream_collect_wire_tile_base_ids(server, top_id, WD_WIRE_TILE_MAX_WIDTH, WD_WIRE_TILE_MAX_HEIGHT,
                                                         ids, &count, (uint16_t)(sizeof(ids) / sizeof(ids[0]))) && count > 0)
                {
                    wd_stream_mark_dirty_top_region_locked(server, ids[0]);
                }
            }
        }

        for (uint16_t ri = 0; ri < job_count; ++ri)
        {
            free(results[ri].payload);
            results[ri].payload = NULL;
        }
        free(results);
        free(jobs);
        free(epoch_snapshot);
        free(dirty_snapshot);

        if (stop_sending)
        {
            break;
        }
    }

    wd_stream_send_retransmits_locked(server, now, 0, tile_bytes, compressed_tile, compressed_capacity);

    /* Keep scene_dirty true if there are queued fresh or repair tiles. */
    server->scene_dirty = net->dirty_region_count > 0 || net->retransmit_queue_count > 0;

    pthread_mutex_unlock(&net->lock);

    free(compressed_tile);
    free(tile_bytes);
    return true;
}

static bool wd_stream_send_generation_summary_kind_locked(struct wd_server* server, bool full_summary) {
    struct wd_net_state* net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0)
    {
        return true;
    }

    const uint64_t summary_build_start_ns = wd_now_ns();
    uint16_t requested_tile_count = full_summary ? server->total_tiles : net->summary_dirty_count;
    if (requested_tile_count == 0)
    {
        return true;
    }

    size_t payload_capacity = sizeof(struct wd_tile_summary_payload_header) +
                              (size_t)server->total_tiles * sizeof(struct wd_tile_generation_entry);

    uint8_t* payload = malloc(payload_capacity);
    if (!payload)
    {
        return false;
    }

    struct wd_tile_generation_entry* entries =
        (struct wd_tile_generation_entry*)(payload + sizeof(struct wd_tile_summary_payload_header));

    uint16_t entry_count = 0;

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        if (!full_summary && (!net->summary_dirty_tiles || !net->summary_dirty_tiles[i]))
        {
            continue;
        }

        entries[entry_count].tile_id           = i;
        entries[entry_count].reserved          = 0;
        entries[entry_count].tile_generation   = net->tiles[i].generation;
        entries[entry_count].tile_timestamp_ns = net->tiles[i].timestamp_ns;
        entry_count++;
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
    header.reserved            = full_summary ? 0u : 1u;

    memcpy(payload, &header, sizeof(header));

    size_t payload_size = sizeof(header) + (size_t)entry_count * sizeof(struct wd_tile_generation_entry);
    net->stats.summary_build_ns += wd_now_ns() - summary_build_start_ns;
    bool ok = wd_send_tcp_message(net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload, (uint32_t)payload_size);

    if (ok)
    {
        if (full_summary)
        {
            if (net->summary_dirty_tiles)
            {
                memset(net->summary_dirty_tiles, 0, server->total_tiles * sizeof(*net->summary_dirty_tiles));
            }
            net->summary_dirty_count = 0;
            net->stats.tcp_summary_full_tx++;
        }
        else
        {
            for (uint16_t i = 0; i < entry_count; ++i)
            {
                uint16_t tile_id = entries[i].tile_id;
                if (tile_id < server->total_tiles && net->summary_dirty_tiles && net->summary_dirty_tiles[tile_id])
                {
                    net->summary_dirty_tiles[tile_id] = false;
                    if (net->summary_dirty_count > 0)
                    {
                        net->summary_dirty_count--;
                    }
                }
            }
            net->stats.tcp_summary_delta_tx++;
            net->stats.tcp_summary_delta_tiles += entry_count;
        }

        if (net->input_since_last_summary && net->last_input_inject_ns != 0 && header.server_timestamp_ns >= net->last_input_inject_ns)
        {
            net->stats.input_to_summary_samples++;
            net->stats.input_to_summary_sum_ns += header.server_timestamp_ns - net->last_input_inject_ns;
            net->input_since_last_summary = false;
        }

        net->stats.tcp_summary_tx++;
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

static double wd_avg_ms(uint64_t sum_ns, uint64_t samples) {
    if (samples == 0)
    {
        return 0.0;
    }

    return (double)sum_ns / (double)samples / 1000000.0;
}

void wd_stream_print_and_reset_stats(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stats s = net->stats;
    memset(&net->stats, 0, sizeof(net->stats));
    wd_stream_policy_update_health_locked(&net->stream_policy, &s);
    uint64_t limited_udp_kib_per_second = net->stream_policy.limited_udp_bytes_per_second / 1024ull;
    uint16_t target_fps = net->stream_policy.target_fps;
    uint16_t effective_target_fps = wd_stream_policy_effective_fps_locked(&net->stream_policy);
    uint16_t tile_width = server->tile_width;
    uint16_t tile_height = server->tile_height;
    bool input_channel_connected = net->input_tcp_fd >= 0;
    bool selection_channel_connected = net->selection_tcp_fd >= 0;

    pthread_mutex_unlock(&net->lock);

    static bool     have_prev_state = false;
    static uint16_t prev_target_fps = 0;
    static uint16_t prev_effective_fps = 0;
    static uint64_t prev_limited_kib = 0;
    static uint16_t prev_tile_width = 0;
    static uint16_t prev_tile_height = 0;
    static bool     prev_input_channel = false;
    static bool     prev_selection_channel = false;

    bool state_changed = !have_prev_state ||
                         prev_target_fps != target_fps || prev_effective_fps != effective_target_fps ||
                         prev_limited_kib != limited_udp_kib_per_second || prev_tile_width != tile_width ||
                         prev_tile_height != tile_height || prev_input_channel != input_channel_connected ||
                         prev_selection_channel != selection_channel_connected;

    if (state_changed)
    {
        WD_LOG_DEBUG("WayDisplay state/s: target_fps=%u effective_fps=%u udp_budget_kib_per_sec=%llu base_tile=%ux%u wire_tiles=128x64,64x64,32x32,16x16 input_channel=%s selection_channel=%s",
                     (unsigned)target_fps, (unsigned)effective_target_fps,
                     (unsigned long long)limited_udp_kib_per_second, (unsigned)tile_width, (unsigned)tile_height,
                     input_channel_connected ? "yes" : "no", selection_channel_connected ? "yes" : "no");

        have_prev_state = true;
        prev_target_fps = target_fps;
        prev_effective_fps = effective_target_fps;
        prev_limited_kib = limited_udp_kib_per_second;
        prev_tile_width = tile_width;
        prev_tile_height = tile_height;
        prev_input_channel = input_channel_connected;
        prev_selection_channel = selection_channel_connected;
    }

    bool video_activity = s.dirty_tiles != 0 || s.dirty_tiles_stale_skipped != 0 || s.udp_tiles_sent != 0 ||
                          s.udp_fresh_tiles_sent != 0 || s.udp_retx_tiles_sent != 0 || s.udp_packets_sent != 0 ||
                          s.udp_bytes_sent != 0 || s.udp_send_pressure_drops != 0 ||
                          s.tile_choice_compressed != 0 || s.tile_choice_uncompressed != 0 ||
                          s.dirty_queue_age_samples != 0 || s.retx_queue_age_samples != 0 ||
                          s.dirty_region_probes != 0 || s.dirty_region_hits != 0 ||
                          s.dirty_budget_blocked != 0 || s.partial_tile_sends != 0 || s.dirty_detect_ns != 0 || s.dirty_region_select_ns != 0 ||
                          s.tile_encode_ns != 0 || s.summary_build_ns != 0 || s.udp_send_ns != 0 || s.encode_jobs_submitted != 0 ||
                          s.encode_jobs_completed != 0 || s.encode_jobs_stale != 0 || s.encode_wait_ns != 0;
    if (video_activity)
    {
        uint64_t choices = s.tile_choice_compressed + s.tile_choice_uncompressed;
        WD_LOG_DEBUG("WayDisplay video/s: dirty=%llu stale_skip=%llu udp_tiles=%llu fresh=%llu retx=%llu pkts=%llu kib=%.1f wire_avg_B=%.1f comp_sent=%llu uncomp_sent=%llu comp_payload_avg_B=%.1f uncomp_payload_avg_B=%.1f choice_comp=%llu choice_uncomp=%llu choice_comp_payload_avg_B=%.1f choice_raw_payload_avg_B=%.1f choice_comp_wire_avg_B=%.1f choice_uncomp_wire_avg_B=%.1f choice_chosen_wire_avg_B=%.1f choice_saved_kib=%.1f pressure_drops=%llu dirty_q_avg_ms=%.2f retx_q_avg_ms=%.2f dirty_region_probes=%llu dirty_region_hits=%llu dirty_budget_blocked=%llu partial_tiles=%llu partial_pkts=%llu detect_ms=%.2f region_pick_ms=%.2f encode_ms=%.2f udp_send_ms=%.2f summary_ms=%.2f encode_jobs=%llu/%llu stale=%llu encode_wait_ms=%.2f encode_worker_ms=%.2f encode_threads=%llu",
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
                     (unsigned long long)s.encode_jobs_completed,
                     (unsigned long long)s.encode_jobs_submitted,
                     (unsigned long long)s.encode_jobs_stale,
                     (double)s.encode_wait_ns / 1000000.0,
                     (double)s.encode_worker_ns / 1000000.0,
                     (unsigned long long)s.encode_threads_used);
    }

    bool repair_activity = s.retx_req_rx != 0 || s.retx_tiles_req != 0 || s.retx_req_ignored_live != 0 ||
                           s.retx_req_stale_generation != 0 || s.retx_req_waiting_for_generation != 0 ||
                           s.retx_tiles_superseded_by_fresh != 0 || s.tcp_summary_tx != 0 || s.tcp_summary_delta_tx != 0 ||
                           s.tcp_summary_delta_tiles != 0 || s.rate_decreases != 0 || s.rate_increases != 0 ||
                           s.frame_rate_downshifts != 0 || s.frame_rate_upshifts != 0;
    if (repair_activity)
    {
        WD_LOG_DEBUG("WayDisplay repair/s: summaries=%llu full=%llu delta=%llu delta_tiles=%llu retx_req=%llu retx_tiles=%llu stale_gen=%llu waiting_gen=%llu ignored_live=%llu superseded=%llu rate_down=%llu rate_up=%llu fps_down=%llu fps_up=%llu",
                     (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.tcp_summary_full_tx,
                     (unsigned long long)s.tcp_summary_delta_tx, (unsigned long long)s.tcp_summary_delta_tiles,
                     (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req,
                     (unsigned long long)s.retx_req_stale_generation, (unsigned long long)s.retx_req_waiting_for_generation,
                     (unsigned long long)s.retx_req_ignored_live, (unsigned long long)s.retx_tiles_superseded_by_fresh,
                     (unsigned long long)s.rate_decreases, (unsigned long long)s.rate_increases,
                     (unsigned long long)s.frame_rate_downshifts, (unsigned long long)s.frame_rate_upshifts);
    }

    bool client_activity = s.client_tiles_completed != 0 || s.client_udp_bytes_rx != 0 || s.client_partial_tiles_timed_out != 0 ||
                           s.client_old_generation_tiles != 0 || s.client_retx_requests_tx != 0 ||
                           s.client_udp_interarrival_samples != 0;
    if (client_activity)
    {
        WD_LOG_DEBUG("WayDisplay client/s: reports=%llu completed=%llu udp_kib=%.1f partial_timeouts=%llu old_gen=%llu retx_req_tx=%llu interarrival_avg_ms=%.2f jitter_avg_ms=%.2f max_gap_ms=%.2f",
                     (unsigned long long)s.client_stats_rx, (unsigned long long)s.client_tiles_completed,
                     (double)s.client_udp_bytes_rx / 1024.0, (unsigned long long)s.client_partial_tiles_timed_out,
                     (unsigned long long)s.client_old_generation_tiles, (unsigned long long)s.client_retx_requests_tx,
                     wd_avg_ms(s.client_udp_interarrival_sum_ns, s.client_udp_interarrival_samples),
                     wd_avg_ms(s.client_udp_interarrival_jitter_sum_ns, s.client_udp_interarrival_jitter_samples),
                     (double)s.client_udp_interarrival_max_ns / 1000000.0);
    }

    bool control_activity = s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.tcp_input_channel_rx != 0 ||
                            s.tcp_input_channel_accepted != 0 || s.tcp_input_channel_closed != 0 ||
                            s.tcp_selection_channel_rx != 0 || s.tcp_selection_channel_accepted != 0 ||
                            s.tcp_selection_channel_closed != 0;
    if (control_activity)
    {
        WD_LOG_DEBUG("WayDisplay control/s: hello=%llu config=%llu input_rx=%llu input_accepted=%llu input_closed=%llu selection_rx=%llu selection_accepted=%llu selection_closed=%llu",
                     (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                     (unsigned long long)s.tcp_input_channel_rx, (unsigned long long)s.tcp_input_channel_accepted,
                     (unsigned long long)s.tcp_input_channel_closed, (unsigned long long)s.tcp_selection_channel_rx,
                     (unsigned long long)s.tcp_selection_channel_accepted, (unsigned long long)s.tcp_selection_channel_closed);
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
        WD_LOG_DEBUG("WayDisplay input/s: key_rx=%llu key_injected=%llu key_dropped=%llu dup_press=%llu release_without_press=%llu keyboard_enter=%llu pointer_rx=%llu pointer_injected=%llu pointer_dropped=%llu grabs_start=%llu grabs_end=%llu grabs_clear=%llu grab_surface_destroyed=%llu queue_avg_ms=%.2f input_to_summary_avg_ms=%.2f input_to_first_tile_avg_ms=%.2f",
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
                               s.cursor_shape_requests != 0 || s.cursor_set_cursor_requests != 0 ||
                               s.cursor_set_cursor_rejected != 0 || s.cursor_set_cursor_hidden != 0 ||
                               s.cursor_set_cursor_fallback != 0;
    if (compositor_activity)
    {
        WD_LOG_DEBUG("WayDisplay compositor/s: xdg_move_bad_serial=%llu xdg_resize_bad_serial=%llu popup_scene=%llu popup_scene_fail=%llu cursor_shape=%llu cursor_set=%llu cursor_reject=%llu cursor_hidden=%llu cursor_fallback=%llu",
                     (unsigned long long)s.xdg_move_invalid_serial, (unsigned long long)s.xdg_resize_invalid_serial,
                     (unsigned long long)s.popup_explicit_scene_trees, (unsigned long long)s.popup_explicit_scene_tree_failures,
                     (unsigned long long)s.cursor_shape_requests, (unsigned long long)s.cursor_set_cursor_requests,
                     (unsigned long long)s.cursor_set_cursor_rejected, (unsigned long long)s.cursor_set_cursor_hidden,
                     (unsigned long long)s.cursor_set_cursor_fallback);
    }
}
