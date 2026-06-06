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

#define WD_NSEC_PER_SEC 1000000000ull

#define WD_UDP_SEND_PRESSURE_LOG_INTERVAL_NS 1000000000ull

static const char* wd_stream_mode_name(uint16_t mode) {
    switch (mode)
    {
    case WD_STREAM_MODE_FULL:
        return "full";
    case WD_STREAM_MODE_PARTIAL:
        return "partial";
    case WD_STREAM_MODE_LIMITED:
        return "limited";
    case WD_STREAM_MODE_LIVE:
        return "live";
    default:
        return "unknown";
    }
}


static uint64_t wd_stream_byte_burst_cap_for_rate(uint64_t bytes_per_second) {
    if (bytes_per_second == 0)
    {
        return 0;
    }

    uint64_t cap = bytes_per_second / WD_STREAM_TOKEN_BURST_DIVISOR;
    if (cap < (uint64_t)WD_UNCOMPRESSED_TILE_BYTES * 2ull)
    {
        cap = (uint64_t)WD_UNCOMPRESSED_TILE_BYTES * 2ull;
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

    policy->requested_mode               = WD_STREAM_MODE_PARTIAL;
    policy->mode                         = WD_STREAM_MODE_PARTIAL;
    policy->target_fps                   = WD_DEFAULT_PARTIAL_FPS;
    policy->throttle_bad_windows         = 0;
    policy->throttle_good_windows        = 0;
    policy->limited_udp_bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_apply_client_hello(struct wd_stream_policy* policy, const struct wd_client_hello_payload* hello) {
    if (!policy || !hello)
    {
        return;
    }

    uint16_t mode = hello->stream_mode;

    if (mode != WD_STREAM_MODE_FULL && mode != WD_STREAM_MODE_PARTIAL && mode != WD_STREAM_MODE_LIMITED)
    {
        mode = WD_STREAM_MODE_PARTIAL;
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

    policy->requested_mode       = mode;
    policy->mode                 = mode;
    policy->target_fps           = fps;
    policy->throttle_bad_windows = 0;
    policy->throttle_good_windows = 0;
    if (policy->limited_udp_bytes_per_second == 0)
    {
        policy->limited_udp_bytes_per_second = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }
    wd_stream_policy_reset_tokens(policy);
}

void wd_stream_policy_set_limited_udp_byte_rate(struct wd_stream_policy* policy, uint64_t bytes_per_second) {
    if (!policy)
    {
        return;
    }

    policy->limited_udp_bytes_per_second = wd_stream_clamp_limited_udp_byte_rate(bytes_per_second);
    policy->limited_udp_byte_tokens      = 0.0;
    policy->last_limited_udp_byte_refill_ns = 0;
}

static void wd_stream_policy_set_effective_mode_locked(struct wd_stream_policy* policy, uint16_t mode) {
    if (!policy || policy->mode == mode)
    {
        return;
    }

    uint16_t old_mode = policy->mode;

    policy->mode                  = mode;
    policy->throttle_bad_windows  = 0;
    policy->throttle_good_windows = 0;
    wd_stream_policy_reset_tokens(policy);

    WD_LOG_INFO("WayDisplay: adaptive stream throttle: %s -> %s requested=%s limited_udp_kib_per_sec=%llu",
                wd_stream_mode_name(old_mode), wd_stream_mode_name(policy->mode), wd_stream_mode_name(policy->requested_mode),
                (unsigned long long)(policy->limited_udp_bytes_per_second / 1024ull));
}

static uint16_t wd_stream_next_lower_mode(uint16_t mode) {
    switch (mode)
    {
    case WD_STREAM_MODE_FULL:
        return WD_STREAM_MODE_PARTIAL;
    case WD_STREAM_MODE_PARTIAL:
        return WD_STREAM_MODE_LIMITED;
    default:
        return mode;
    }
}

static uint16_t wd_stream_next_higher_mode(uint16_t mode, uint16_t requested_mode) {
    if (mode == WD_STREAM_MODE_LIMITED && requested_mode <= WD_STREAM_MODE_PARTIAL)
    {
        return WD_STREAM_MODE_PARTIAL;
    }

    if (mode == WD_STREAM_MODE_PARTIAL && requested_mode == WD_STREAM_MODE_FULL)
    {
        return WD_STREAM_MODE_FULL;
    }

    return mode;
}

static void wd_stream_policy_update_adaptive_locked(struct wd_stream_policy* policy, const struct wd_stats* stats) {
    if (!policy || !stats || policy->requested_mode == WD_STREAM_MODE_LIVE || policy->requested_mode == WD_STREAM_MODE_LIMITED)
    {
        return;
    }

    bool send_pressure = stats->udp_send_pressure_drops != 0;

    uint64_t fresh_tiles_for_ratio = stats->dirty_tiles;
    if (fresh_tiles_for_ratio < WD_THROTTLE_MIN_FRESH_TILES_FOR_RATIO)
    {
        fresh_tiles_for_ratio = WD_THROTTLE_MIN_FRESH_TILES_FOR_RATIO;
    }

    bool repair_ratio_high =
        stats->retx_tiles_req * WD_THROTTLE_REPAIR_RATIO_DENOMINATOR >= fresh_tiles_for_ratio * WD_THROTTLE_REPAIR_RATIO_NUMERATOR;

    bool repair_storm = stats->retx_req_rx >= WD_THROTTLE_REPAIR_STORM_REQUESTS_PER_SEC &&
                        stats->retx_tiles_req >= WD_THROTTLE_REPAIR_STORM_TILES_PER_SECOND;

    bool bad_window = send_pressure || repair_storm || (stats->retx_req_rx != 0 && repair_ratio_high);

    if (bad_window)
    {
        policy->throttle_good_windows = 0;
        policy->throttle_bad_windows++;

        uint32_t downgrade_windows = send_pressure ? WD_THROTTLE_PRESSURE_DOWNGRADE_WINDOWS : WD_THROTTLE_BAD_WINDOWS_TO_DOWNGRADE;
        if (policy->throttle_bad_windows >= downgrade_windows)
        {
            uint16_t lower_mode = wd_stream_next_lower_mode(policy->mode);
            if (lower_mode != policy->mode)
            {
                wd_stream_policy_set_effective_mode_locked(policy, lower_mode);
            }
        }

        return;
    }

    policy->throttle_bad_windows = 0;

    if (policy->mode != policy->requested_mode)
    {
        policy->throttle_good_windows++;
        if (policy->throttle_good_windows >= WD_THROTTLE_GOOD_WINDOWS_TO_UPGRADE)
        {
            uint16_t higher_mode = wd_stream_next_higher_mode(policy->mode, policy->requested_mode);
            if (higher_mode != policy->mode)
            {
                wd_stream_policy_set_effective_mode_locked(policy, higher_mode);
            }
        }
    }
}

bool wd_stream_policy_should_render_now(struct wd_server* server, uint64_t now_ns) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    bool                     full_frame_needed = net->full_frame_needed;
    bool                     client_connected  = net->client_connected;
    struct wd_stream_policy* policy            = &net->stream_policy;

    if (!client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    if (!server->scene_dirty && !full_frame_needed)
    {
        pthread_mutex_unlock(&net->lock);
        return false;
    }

    bool should = false;

    switch (policy->mode)
    {
    case WD_STREAM_MODE_FULL:
        should = true;
        break;

    case WD_STREAM_MODE_PARTIAL: {
        uint16_t fps = policy->target_fps ? policy->target_fps : WD_DEFAULT_PARTIAL_FPS;

        uint64_t interval_ns = 1000000000ull / fps;

        if (policy->last_frame_send_ns == 0 || now_ns - policy->last_frame_send_ns >= interval_ns || full_frame_needed)
        {
            policy->last_frame_send_ns = now_ns;
            should                     = true;
        }

        break;
    }

    case WD_STREAM_MODE_LIMITED:
    /*
     * Limited mode is byte-budget based, but we still need to render
     * periodically so we can discover changed tiles.
     *
     * Use a 30fps discovery cadence by default.
     */
    default: {
        uint64_t interval_ns = 1000000000ull / WD_DEFAULT_PARTIAL_FPS;

        if (policy->last_frame_send_ns == 0 || now_ns - policy->last_frame_send_ns >= interval_ns || full_frame_needed)
        {
            policy->last_frame_send_ns = now_ns;
            should                     = true;
        }

        break;
    }
    }

    pthread_mutex_unlock(&net->lock);

    return should;
}

bool wd_stream_init(struct wd_server* server) {
    const size_t compressed_capacity = wd_zstd_compress_bound(WD_UNCOMPRESSED_TILE_BYTES);

    server->net.tiles = calloc(server->total_tiles, sizeof(*server->net.tiles));
    if (!server->net.tiles)
    {
        return false;
    }

    server->damage_tiles = calloc(server->total_tiles, sizeof(*server->damage_tiles));
    if (!server->damage_tiles)
    {
        wd_stream_destroy(server);
        return false;
    }
    server->damage_all_tiles  = true;
    server->damage_tile_count = 0;

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        struct wd_cached_tile* tile = &server->net.tiles[i];

        memset(tile, 0, sizeof(*tile));

        tile->compressed_capacity = (uint32_t)compressed_capacity;
        tile->compressed          = malloc(compressed_capacity);

        if (!tile->compressed)
        {
            wd_stream_destroy(server);
            return false;
        }
    }

    return true;
}

void wd_stream_destroy(struct wd_server* server) {
    if (!server || !server->net.tiles)
    {
        return;
    }

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        free(server->net.tiles[i].compressed);
        server->net.tiles[i].compressed          = NULL;
        server->net.tiles[i].compressed_size     = 0;
        server->net.tiles[i].compressed_capacity = 0;
    }

    free(server->net.tiles);
    server->net.tiles = NULL;

    free(server->damage_tiles);
    server->damage_tiles      = NULL;
    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
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

static uint32_t wd_stream_tile_wire_bytes(uint32_t compressed_size, uint16_t udp_payload_target) {
    if (compressed_size == 0)
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

    uint32_t packet_count = (compressed_size + udp_payload_target - 1u) / udp_payload_target;
    return compressed_size + packet_count * (uint32_t)sizeof(struct wd_udp_tile_packet_header);
}

static uint64_t wd_stream_policy_limited_byte_budget_locked(struct wd_stream_policy* policy, uint64_t now_ns) {
    if (!policy || policy->mode != WD_STREAM_MODE_LIMITED)
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
    if (!policy || policy->mode != WD_STREAM_MODE_LIMITED || bytes == 0)
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

static bool wd_stream_send_tile_payload_locked(struct wd_server* server, uint16_t tile_id, uint64_t generation, uint64_t timestamp_ns,
                                               const uint8_t* compressed, uint32_t compressed_size, bool* launched,
                                               bool* send_blocked, uint32_t* bytes_sent) {
    struct wd_net_state* net = &server->net;

    if (launched)
    {
        *launched = false;
    }

    if (send_blocked)
    {
        *send_blocked = false;
    }

    if (bytes_sent)
    {
        *bytes_sent = 0;
    }

    if (tile_id >= server->total_tiles)
    {
        return false;
    }

    if (!net->client_connected || compressed_size == 0 || !compressed)
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

    uint16_t packet_count = (uint16_t)((compressed_size + udp_payload_target - 1) / udp_payload_target);

    for (uint16_t packet_id = 0; packet_id < packet_count; ++packet_id)
    {
        uint32_t offset = (uint32_t)packet_id * udp_payload_target;

        uint16_t payload_size =
            (uint16_t)(((compressed_size - offset) > udp_payload_target) ? udp_payload_target : (compressed_size - offset));

        struct wd_udp_tile_packet_header h;
        memset(&h, 0, sizeof(h));

        h.tile_id              = tile_id;
        h.tile_pkt_count       = packet_count;
        h.tile_pkt_id          = packet_id;
        h.payload_size         = payload_size;
        h.tile_generation      = generation;
        h.compressed_tile_size = compressed_size;
        h.tile_timestamp_ns    = timestamp_ns;

        struct iovec iov[2];
        memset(iov, 0, sizeof(iov));
        iov[0].iov_base = &h;
        iov[0].iov_len  = sizeof(h);
        iov[1].iov_base = (uint8_t*)compressed + offset;
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
                if (send_blocked)
                {
                    *send_blocked = true;
                }
                break;
            }

            WD_LOG_ERROR("WayDisplay: sendto failed: %s", strerror(errno));
            return false;
        }

        if (launched)
        {
            *launched = true;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)sent;
        if (bytes_sent)
        {
            *bytes_sent += (uint32_t)sent;
        }
    }

    if (launched && *launched)
    {
        net->stats.udp_tiles_sent++;
    }

    return true;
}

bool wd_stream_send_cached_tile_locked(struct wd_server* server, uint16_t tile_id, bool* launched, bool* send_blocked, uint32_t* bytes_sent) {
    struct wd_net_state* net = &server->net;

    if (tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_cached_tile* cache = &net->tiles[tile_id];

    return wd_stream_send_tile_payload_locked(server, tile_id, cache->generation, cache->timestamp_ns, cache->compressed,
                                              cache->compressed_size, launched, send_blocked, bytes_sent);
}

static uint32_t wd_tile_queue_random_locked(struct wd_net_state* net) {
    if (!net)
    {
        return 0;
    }

    uint32_t x = net->tile_queue_rng_state;
    if (x == 0)
    {
        x = 0x9e3779b9u;
    }

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    net->tile_queue_rng_state = x ? x : 0x9e3779b9u;

    return net->tile_queue_rng_state;
}

static bool wd_dirty_queue_push_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || tile_id >= total_tiles)
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

    /*
     * Fresh content supersedes any queued repair for the same tile. Leave the
     * stale repair entry in-place; the repair popper will discard it when it
     * sees retransmit_queued[tile_id] is no longer set.
     */
    if (net->retransmit_queued)
    {
        net->retransmit_queued[tile_id] = false;
    }

    net->dirty_queue[net->dirty_queue_count++] = tile_id;
    net->dirty_queued[tile_id] = true;

    return true;
}

static bool wd_dirty_queue_pop_random_locked(struct wd_net_state* net, uint16_t* out_tile_id, uint16_t total_tiles) {
    if (!net || !out_tile_id)
    {
        return false;
    }

    while (net->dirty_queue_count > 0)
    {
        uint16_t index = (uint16_t)(wd_tile_queue_random_locked(net) % net->dirty_queue_count);
        uint16_t tile_id = net->dirty_queue[index];

        net->dirty_queue_count--;
        net->dirty_queue[index] = net->dirty_queue[net->dirty_queue_count];

        if (tile_id >= total_tiles || !net->dirty_queued[tile_id])
        {
            continue;
        }

        net->dirty_queued[tile_id] = false;
        *out_tile_id = tile_id;
        return true;
    }

    return false;
}

static bool wd_dirty_queue_reinsert_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
    if (!net || tile_id >= total_tiles)
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

    net->dirty_queue[net->dirty_queue_count++] = tile_id;
    net->dirty_queued[tile_id] = true;

    return true;
}

bool wd_stream_queue_retransmit_tile_locked(struct wd_server* server, uint16_t tile_id) {
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
        return false;
    }

    if (net->retransmit_queued[tile_id])
    {
        return false;
    }

    if (net->retransmit_queue_count >= server->total_tiles)
    {
        return false;
    }

    net->retransmit_queue[net->retransmit_queue_count++] = tile_id;
    net->retransmit_queued[tile_id] = true;
    server->scene_dirty = true;

    return true;
}

static bool wd_retransmit_queue_pop_random_locked(struct wd_server* server, uint16_t* out_tile_id) {
    if (!server || !out_tile_id)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    while (net->retransmit_queue_count > 0)
    {
        uint16_t index = (uint16_t)(wd_tile_queue_random_locked(net) % net->retransmit_queue_count);
        uint16_t tile_id = net->retransmit_queue[index];

        net->retransmit_queue_count--;
        net->retransmit_queue[index] = net->retransmit_queue[net->retransmit_queue_count];

        if (tile_id >= server->total_tiles || !net->retransmit_queued[tile_id])
        {
            continue;
        }

        net->retransmit_queued[tile_id] = false;

        if (net->dirty_queued && net->dirty_queued[tile_id])
        {
            continue;
        }

        *out_tile_id = tile_id;
        return true;
    }

    return false;
}

static void wd_clear_damage_tiles(struct wd_server* server) {
    if (!server || !server->damage_tiles)
    {
        return;
    }

    if (server->damage_tile_count > 0)
    {
        memset(server->damage_tiles, 0, (size_t)server->total_tiles * sizeof(*server->damage_tiles));
    }

    server->damage_all_tiles  = false;
    server->damage_tile_count = 0;
}

static void wd_detect_one_dirty_tile_into_queue_locked(struct wd_server* server, uint16_t tile_id) {
    struct wd_net_state* net = &server->net;

    if (tile_id >= server->total_tiles)
    {
        return;
    }

    uint32_t hash = wd_fnv1a_tile_hash_xrgb8888_for(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                    server->tiles_x, server->total_tiles, tile_id);

    if (hash != net->tiles[tile_id].last_hash)
    {
        wd_dirty_queue_push_locked(net, tile_id, server->total_tiles);
    }
}

static void wd_detect_dirty_tiles_into_queue_locked(struct wd_server* server) {
    /*
     * Hash only tiles intersecting compositor-side damage when we know it.
     * Full-frame damage is still used for first frames, resized displays, and
     * any legacy mark path that cannot describe a smaller rectangle safely.
     */
    if (!server->damage_all_tiles && server->damage_tiles && server->damage_tile_count > 0)
    {
        for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
        {
            if (!server->damage_tiles[tile_id])
            {
                continue;
            }

            wd_detect_one_dirty_tile_into_queue_locked(server, tile_id);
        }

        wd_clear_damage_tiles(server);
        return;
    }

    for (uint16_t tile_id = 0; tile_id < server->total_tiles; ++tile_id)
    {
        wd_detect_one_dirty_tile_into_queue_locked(server, tile_id);
    }

    wd_clear_damage_tiles(server);
}

bool wd_stream_send_dirty_tiles(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    if (!server->framebuffer_xrgb8888)
    {
        return false;
    }

    const uint64_t now = wd_now_ns();

    uint8_t  tile_bytes[WD_UNCOMPRESSED_TILE_BYTES];
    size_t   compressed_capacity = wd_zstd_compress_bound(WD_UNCOMPRESSED_TILE_BYTES);
    uint8_t* compressed_tile     = malloc(compressed_capacity);

    if (!compressed_tile)
    {
        return false;
    }

    pthread_mutex_lock(&net->lock);

    if (!net->client_connected)
    {
        pthread_mutex_unlock(&net->lock);
        free(compressed_tile);
        return true;
    }

    /*
     * Phase 1:
     * Progressive full-frame catch-up for new/reconnected clients.
     *
     * This sends each tile once, advancing full_frame_next_tile across
     * multiple limited-mode passes. It does not restart at tile 0.
     */
    if (net->full_frame_needed)
    {
        while (net->full_frame_next_tile < server->total_tiles)
        {
            uint16_t               tile_id = net->full_frame_next_tile;
            struct wd_cached_tile* tile    = &net->tiles[tile_id];

            uint32_t hash = wd_fnv1a_tile_hash_xrgb8888_for(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                            server->tiles_x, server->total_tiles, tile_id);

            bool cache_valid = tile->compressed_size > 0 && tile->compressed;

            if (!cache_valid || hash != tile->last_hash)
            {
                if (!wd_extract_tile_xrgb8888_for(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                  server->tiles_x, server->total_tiles, tile_id, tile_bytes))
                {
                    continue;
                }

                uint32_t compressed_size = 0;

                if (!wd_zstd_compress(tile_bytes, WD_UNCOMPRESSED_TILE_BYTES, tile->compressed, tile->compressed_capacity, WD_ZSTD_LEVEL,
                                      &compressed_size))
                {
                    WD_LOG_ERROR("WayDisplay: zstd compression failed for full-frame tile %u", tile_id);
                    continue;
                }

                tile->last_hash = hash;
                tile->generation++;
                tile->timestamp_ns    = now;
                tile->compressed_size = compressed_size;

                net->stats.dirty_tiles++;
            }

            bool     launched     = false;
            bool     send_blocked = false;
            uint32_t bytes_sent    = 0;

            uint32_t predicted_bytes = wd_stream_tile_wire_bytes(tile->compressed_size, net->udp_payload_target);
            if (wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now) < predicted_bytes)
            {
                break;
            }

            if (!wd_stream_send_cached_tile_locked(server, tile_id, &launched, &send_blocked, &bytes_sent))
            {
                continue;
            }

            if (!launched && send_blocked)
            {
                break;
            }

            net->full_frame_next_tile++;

            if (launched)
            {
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes_sent);
            }

            if (send_blocked)
            {
                break;
            }
        }

        if (net->full_frame_next_tile >= server->total_tiles)
        {
            net->full_frame_needed    = false;
            net->full_frame_next_tile = 0;
            net->dirty_scan_next_tile = 0;

            /*
             * After initial catch-up, clear stale queue state and force the
             * next normal pass to detect current dirty tiles from hashes.
             */
            if (net->dirty_queued)
            {
                memset(net->dirty_queued, 0, server->total_tiles * sizeof(*net->dirty_queued));
            }
            if (net->retransmit_queued)
            {
                memset(net->retransmit_queued, 0, server->total_tiles * sizeof(*net->retransmit_queued));
            }
            net->dirty_queue_read       = 0;
            net->dirty_queue_write      = 0;
            net->dirty_queue_count      = 0;
            net->retransmit_queue_count = 0;

            wd_clear_damage_tiles(server);
            server->scene_dirty = false;
        }
        else
        {
            server->scene_dirty = true;
        }

        pthread_mutex_unlock(&net->lock);

        free(compressed_tile);
        return true;
    }

    /*
     * Phase 2:
     * Normal mode. Detect new dirty work, then spend the byte budget on
     * latest-generation repairs before fresh tiles. Repairing already-missing
     * tiles first avoids priority inversion and lets the remaining byte budget
     * carry new work.
     */
    wd_detect_dirty_tiles_into_queue_locked(server);

    while (net->retransmit_queue_count > 0)
    {
            uint16_t tile_id = 0;

            if (!wd_retransmit_queue_pop_random_locked(server, &tile_id))
            {
                break;
            }

            if (tile_id >= server->total_tiles)
            {
                continue;
            }

            if (net->dirty_queued && net->dirty_queued[tile_id])
            {
                continue;
            }

            bool     launched     = false;
            bool     send_blocked = false;
            uint32_t bytes_sent    = 0;

            struct wd_cached_tile* cache = &net->tiles[tile_id];
            uint32_t predicted_bytes = wd_stream_tile_wire_bytes(cache->compressed_size, net->udp_payload_target);
            if (wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now) < predicted_bytes)
            {
                wd_stream_queue_retransmit_tile_locked(server, tile_id);
                break;
            }

            if (!wd_stream_send_cached_tile_locked(server, tile_id, &launched, &send_blocked, &bytes_sent))
            {
                continue;
            }

            if (send_blocked && !launched)
            {
                wd_stream_queue_retransmit_tile_locked(server, tile_id);
                break;
            }

            if (launched)
            {
                wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes_sent);
            }

            if (send_blocked)
            {
                break;
            }
    }

    while (net->dirty_queue_count > 0)
    {
        uint16_t tile_id = 0;

        if (!wd_dirty_queue_pop_random_locked(net, &tile_id, server->total_tiles))
        {
            break;
        }

        if (tile_id >= server->total_tiles)
        {
            continue;
        }

        struct wd_cached_tile* tile = &net->tiles[tile_id];

        uint32_t hash = wd_fnv1a_tile_hash_xrgb8888_for(server->framebuffer_xrgb8888, server->display_width, server->display_height,
                                                        server->tiles_x, server->total_tiles, tile_id);

        /*
         * It may have been queued earlier but become identical by now.
         */
        if (hash == tile->last_hash)
        {
            continue;
        }

        if (!wd_extract_tile_xrgb8888_for(server->framebuffer_xrgb8888, server->display_width, server->display_height, server->tiles_x,
                                          server->total_tiles, tile_id, tile_bytes))
        {
            continue;
        }

        uint32_t compressed_size = 0;

        if (!wd_zstd_compress(tile_bytes, WD_UNCOMPRESSED_TILE_BYTES, compressed_tile, (uint32_t)compressed_capacity, WD_ZSTD_LEVEL,
                              &compressed_size))
        {
            WD_LOG_ERROR("WayDisplay: zstd compression failed for dirty tile %u", tile_id);
            continue;
        }

        uint64_t next_generation = tile->generation + 1;
        bool     launched        = false;
        bool     send_blocked    = false;
        uint32_t bytes_sent       = 0;

        uint32_t predicted_bytes = wd_stream_tile_wire_bytes(compressed_size, net->udp_payload_target);
        if (wd_stream_policy_limited_byte_budget_locked(&net->stream_policy, now) < predicted_bytes)
        {
            wd_dirty_queue_reinsert_locked(net, tile_id, server->total_tiles);
            break;
        }

        if (!wd_stream_send_tile_payload_locked(server, tile_id, next_generation, now, compressed_tile, compressed_size, &launched,
                                                &send_blocked, &bytes_sent))
        {
            continue;
        }

        if (!launched)
        {
            wd_dirty_queue_reinsert_locked(net, tile_id, server->total_tiles);
            break;
        }

        memcpy(tile->compressed, compressed_tile, compressed_size);
        tile->last_hash       = hash;
        tile->generation      = next_generation;
        tile->timestamp_ns    = now;
        tile->compressed_size = compressed_size;

        net->stats.dirty_tiles++;
        wd_stream_policy_consume_limited_bytes_locked(&net->stream_policy, bytes_sent);

        if (send_blocked)
        {
            break;
        }
    }

    /*
     * Keep scene_dirty true if there are queued fresh or repair tiles.
     * Repairs are drained before fresh work so missing visible tiles do not get
     * starved behind newly dirtied tiles.
     */
    server->scene_dirty = net->dirty_queue_count > 0 || net->retransmit_queue_count > 0;

    pthread_mutex_unlock(&net->lock);

    free(compressed_tile);
    return true;
}

bool wd_stream_send_generation_summary_locked(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0)
    {
        return true;
    }

    struct wd_tile_summary_payload_header header;

    header.session_id          = net->session_id;
    header.server_timestamp_ns = wd_now_ns();
    header.tile_count          = server->total_tiles;
    header.reserved            = 0;

    size_t payload_size = sizeof(header) + server->total_tiles * sizeof(struct wd_tile_generation_entry);

    uint8_t* payload = malloc(payload_size);
    if (!payload)
    {
        return false;
    }

    memcpy(payload, &header, sizeof(header));

    struct wd_tile_generation_entry* entries = (struct wd_tile_generation_entry*)(payload + sizeof(header));

    for (uint16_t i = 0; i < server->total_tiles; ++i)
    {
        entries[i].tile_id           = i;
        entries[i].reserved          = 0;
        entries[i].tile_generation   = net->tiles[i].generation;
        entries[i].tile_timestamp_ns = net->tiles[i].timestamp_ns;
    }

    bool ok = wd_send_tcp_message(net->tcp_fd, WD_MSG_TILE_GENERATION_SUMMARY, payload, (uint32_t)payload_size);

    free(payload);

    if (ok)
    {
        if (net->input_since_last_summary && net->last_input_inject_ns != 0 && header.server_timestamp_ns >= net->last_input_inject_ns)
        {
            net->stats.input_to_summary_samples++;
            net->stats.input_to_summary_sum_ns += header.server_timestamp_ns - net->last_input_inject_ns;
            net->input_since_last_summary = false;
        }

        net->stats.tcp_summary_tx++;
    }

    return ok;
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
    wd_stream_policy_update_adaptive_locked(&net->stream_policy, &s);
    uint16_t effective_mode = net->stream_policy.mode;
    uint16_t requested_mode = net->stream_policy.requested_mode;
    uint64_t limited_udp_kib_per_second = net->stream_policy.limited_udp_bytes_per_second / 1024ull;

    pthread_mutex_unlock(&net->lock);

    /*
     * Keepalive generation summaries are expected while idle. Suppress the
     * stats line when they are the only activity so lossy-link repair stays
     * enabled without spamming logs.
     */
    bool useful_activity = s.dirty_tiles != 0 || s.udp_tiles_sent != 0 || s.udp_packets_sent != 0 || s.udp_bytes_sent != 0 ||
                           s.udp_send_pressure_drops != 0 || s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.retx_req_rx != 0 || s.retx_tiles_req != 0 ||
                           s.key_events_rx != 0 || s.key_events_injected != 0 || s.key_events_dropped != 0 || s.pointer_events_rx != 0 ||
                           s.pointer_events_injected != 0 || s.pointer_events_dropped != 0;

    if (!useful_activity)
    {
        return;
    }

    WD_LOG_DEBUG("WayDisplay stats/s: mode=%s requested_mode=%s limited_udp_kib_per_sec=%llu "
                 "dirty_tiles=%llu udp_tiles_sent=%llu udp_pkts=%llu udp_kib=%.1f udp_pressure_drops=%llu "
                 "tcp_hello_rx=%llu tcp_cfg_tx=%llu tcp_summary_tx=%llu retx_req_rx=%llu "
                 "retx_tiles_req=%llu "
                 "key_rx=%llu key_injected=%llu key_dropped=%llu pointer_rx=%llu pointer_injected=%llu "
                 "pointer_dropped=%llu input_net_avg_ms=n/a input_queue_avg_ms=%.2f "
                 "input_to_summary_avg_ms=%.2f",
                 wd_stream_mode_name(effective_mode), wd_stream_mode_name(requested_mode),
                 (unsigned long long)limited_udp_kib_per_second,
                 (unsigned long long)s.dirty_tiles, (unsigned long long)s.udp_tiles_sent, (unsigned long long)s.udp_packets_sent,
                 (double)s.udp_bytes_sent / 1024.0, (unsigned long long)s.udp_send_pressure_drops,
                 (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                 (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req,
                 (unsigned long long)s.key_events_rx, (unsigned long long)s.key_events_injected, (unsigned long long)s.key_events_dropped,
                 (unsigned long long)s.pointer_events_rx, (unsigned long long)s.pointer_events_injected,
                 (unsigned long long)s.pointer_events_dropped, wd_avg_ms(s.input_queue_latency_sum_ns, s.input_queue_latency_samples),
                 wd_avg_ms(s.input_to_summary_sum_ns, s.input_to_summary_samples));
}
