#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"
#include "wd_server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define WD_UDP_SEND_PRESSURE_LOG_INTERVAL_NS 1000000000ull

void wd_stream_policy_set_defaults(struct wd_stream_policy* policy) {
    if (!policy)
    {
        return;
    }

    memset(policy, 0, sizeof(*policy));

    policy->mode                            = WD_STREAM_MODE_PARTIAL;
    policy->target_fps                      = WD_DEFAULT_PARTIAL_FPS;
    policy->max_tiles_per_second            = WD_DEFAULT_LIMITED_TILES_PER_SECOND;
    policy->max_retransmit_tiles_per_second = WD_DEFAULT_RETRANSMIT_TILES_PER_SECOND;
    policy->retransmit_tile_tokens          = 0.0;
    policy->last_retransmit_token_refill_ns = 0;
    policy->last_frame_send_ns              = 0;
    policy->tile_tokens                     = 0.0;
    policy->last_token_refill_ns            = 0;
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

    uint32_t max_tiles = hello->max_tiles_per_second;
    if (max_tiles == 0)
    {
        max_tiles = WD_DEFAULT_LIMITED_TILES_PER_SECOND;
    }

    if (max_tiles > WD_MAX_REASONABLE_TILES_PER_SECOND)
    {
        max_tiles = WD_MAX_REASONABLE_TILES_PER_SECOND;
    }

    uint32_t retx_tiles = WD_DEFAULT_RETRANSMIT_TILES_PER_SECOND;

    switch (mode)
    {
    case WD_STREAM_MODE_FULL:
        /*
         * Full mode can repair faster, but still cap repair so it does not
         * dominate fresh updates.
         */
        retx_tiles = WD_FULL_MODE_RETRANSMIT_TILES_PER_SECOND;
        break;

    case WD_STREAM_MODE_PARTIAL:
        retx_tiles = WD_PARTIAL_MODE_RETRANSMIT_TILES_PER_SECOND;
        break;

    case WD_STREAM_MODE_LIMITED:
        /*
         * Limited mode: retransmit gets roughly 20% of the fresh tile budget,
         * with a small floor.
         */
        retx_tiles = max_tiles / WD_LIMITED_MODE_RETRANSMIT_DIVISOR;
        if (retx_tiles < WD_LIMITED_MODE_RETRANSMIT_MIN_TILES_PER_SEC)
        {
            retx_tiles = WD_LIMITED_MODE_RETRANSMIT_MIN_TILES_PER_SEC;
        }
        break;

    default:
        retx_tiles = WD_DEFAULT_RETRANSMIT_TILES_PER_SECOND;
        break;
    }

    if (retx_tiles > WD_MAX_REASONABLE_RETRANSMIT_TILES_PER_SECOND)
    {
        retx_tiles = WD_MAX_REASONABLE_RETRANSMIT_TILES_PER_SECOND;
    }

    policy->mode                            = mode;
    policy->target_fps                      = fps;
    policy->max_tiles_per_second            = max_tiles;
    policy->max_retransmit_tiles_per_second = retx_tiles;
    policy->retransmit_tile_tokens          = 0.0;
    policy->last_retransmit_token_refill_ns = 0;
    policy->last_frame_send_ns              = 0;
    policy->tile_tokens                     = 0.0;
    policy->last_token_refill_ns            = 0;
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
     * Limited mode is tile-budget based, but we still need to render
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

uint32_t wd_stream_policy_tile_budget(struct wd_server* server, uint64_t now_ns) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stream_policy* policy = &net->stream_policy;

    if (policy->mode != WD_STREAM_MODE_LIMITED)
    {
        pthread_mutex_unlock(&net->lock);
        return server->total_tiles;
    }

    if (policy->last_token_refill_ns == 0)
    {
        policy->last_token_refill_ns = now_ns;
        policy->tile_tokens          = (double)policy->max_tiles_per_second;
    }
    else
    {
        uint64_t elapsed_ns      = now_ns - policy->last_token_refill_ns;
        double   elapsed_seconds = (double)elapsed_ns / 1000000000.0;

        policy->tile_tokens += elapsed_seconds * (double)policy->max_tiles_per_second;

        /*
         * Allow one second of burst.
         */
        if (policy->tile_tokens > (double)policy->max_tiles_per_second)
        {
            policy->tile_tokens = (double)policy->max_tiles_per_second;
        }

        policy->last_token_refill_ns = now_ns;
    }

    uint32_t budget = (uint32_t)policy->tile_tokens;

    pthread_mutex_unlock(&net->lock);

    return budget;
}

void wd_stream_policy_consume_tiles(struct wd_server* server, uint32_t count) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stream_policy* policy = &net->stream_policy;

    if (policy->mode == WD_STREAM_MODE_LIMITED)
    {
        if (policy->tile_tokens >= (double)count)
        {
            policy->tile_tokens -= (double)count;
        }
        else
        {
            policy->tile_tokens = 0.0;
        }
    }

    pthread_mutex_unlock(&net->lock);
}

uint32_t wd_stream_policy_retransmit_budget(struct wd_server* server, uint64_t now_ns) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stream_policy* policy = &net->stream_policy;

    uint32_t rate = policy->max_retransmit_tiles_per_second;
    if (rate == 0)
    {
        pthread_mutex_unlock(&net->lock);
        return 0;
    }

    if (policy->last_retransmit_token_refill_ns == 0)
    {
        policy->last_retransmit_token_refill_ns = now_ns;
        policy->retransmit_tile_tokens          = (double)rate;
    }
    else
    {
        uint64_t elapsed_ns = now_ns - policy->last_retransmit_token_refill_ns;

        double elapsed_seconds = (double)elapsed_ns / 1000000000.0;

        policy->retransmit_tile_tokens += elapsed_seconds * (double)rate;

        /*
         * Allow one second of retransmit burst.
         */
        if (policy->retransmit_tile_tokens > (double)rate)
        {
            policy->retransmit_tile_tokens = (double)rate;
        }

        policy->last_retransmit_token_refill_ns = now_ns;
    }

    uint32_t budget = (uint32_t)policy->retransmit_tile_tokens;

    pthread_mutex_unlock(&net->lock);

    return budget;
}

void wd_stream_policy_consume_retransmit_tiles(struct wd_server* server, uint32_t count) {
    struct wd_net_state* net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stream_policy* policy = &net->stream_policy;

    if (policy->retransmit_tile_tokens >= (double)count)
    {
        policy->retransmit_tile_tokens -= (double)count;
    }
    else
    {
        policy->retransmit_tile_tokens = 0.0;
    }

    pthread_mutex_unlock(&net->lock);
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

static bool wd_stream_send_tile_payload_locked(struct wd_server* server, uint16_t tile_id, uint64_t generation, uint64_t timestamp_ns,
                                               const uint8_t* compressed, uint32_t compressed_size, bool* launched) {
    struct wd_net_state* net = &server->net;

    if (launched)
    {
        *launched = false;
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
                continue;
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
    }

    if (!launched || *launched)
    {
        net->stats.udp_tiles_sent++;
    }

    return true;
}

bool wd_stream_send_cached_tile_locked(struct wd_server* server, uint16_t tile_id) {
    struct wd_net_state* net = &server->net;

    if (tile_id >= server->total_tiles)
    {
        return false;
    }

    struct wd_cached_tile* cache = &net->tiles[tile_id];

    return wd_stream_send_tile_payload_locked(server, tile_id, cache->generation, cache->timestamp_ns, cache->compressed,
                                              cache->compressed_size, NULL);
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
        /*
         * Queue is full. This should not happen because each tile can only
         * appear once, but keep it safe.
         */
        return false;
    }

    net->dirty_queue[net->dirty_queue_write] = tile_id;
    net->dirty_queue_write                   = (uint16_t)((net->dirty_queue_write + 1) % total_tiles);
    net->dirty_queue_count++;
    net->dirty_queued[tile_id] = true;

    return true;
}

static bool wd_dirty_queue_pop_locked(struct wd_net_state* net, uint16_t* out_tile_id, uint16_t total_tiles) {
    if (!net || !out_tile_id || net->dirty_queue_count == 0)
    {
        return false;
    }

    uint16_t tile_id = net->dirty_queue[net->dirty_queue_read];

    net->dirty_queue_read = (uint16_t)((net->dirty_queue_read + 1) % total_tiles);
    net->dirty_queue_count--;

    if (tile_id < total_tiles)
    {
        net->dirty_queued[tile_id] = false;
    }

    *out_tile_id = tile_id;
    return true;
}

static bool wd_dirty_queue_reinsert_front_locked(struct wd_net_state* net, uint16_t tile_id, uint16_t total_tiles) {
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

    net->dirty_queue_read = (uint16_t)((net->dirty_queue_read + total_tiles - 1) % total_tiles);
    net->dirty_queue[net->dirty_queue_read] = tile_id;
    net->dirty_queue_count++;
    net->dirty_queued[tile_id] = true;

    return true;
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

    const uint64_t now                  = wd_now_ns();
    uint32_t       tile_budget          = wd_stream_policy_tile_budget(server, now);
    uint32_t       tiles_sent_this_pass = 0;

    if (tile_budget == 0)
    {
        return true;
    }

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
        while (net->full_frame_next_tile < server->total_tiles && tiles_sent_this_pass < tile_budget)
        {
            uint16_t               tile_id = net->full_frame_next_tile++;
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

            wd_stream_send_cached_tile_locked(server, tile_id);
            tiles_sent_this_pass++;
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
            net->dirty_queue_read  = 0;
            net->dirty_queue_write = 0;
            net->dirty_queue_count = 0;

            wd_clear_damage_tiles(server);
            server->scene_dirty = false;
        }
        else
        {
            server->scene_dirty = true;
        }

        pthread_mutex_unlock(&net->lock);

        wd_stream_policy_consume_tiles(server, tiles_sent_this_pass);

        free(compressed_tile);
        return true;
    }

    /*
     * Phase 2:
     * Normal dirty-tile mode.
     *
     * First detect changed tiles and enqueue them once. Then drain the queue
     * according to the active budget. This avoids top-left scan bias and
     * avoids repeatedly inserting the same tile while it waits for bandwidth.
     */
    wd_detect_dirty_tiles_into_queue_locked(server);

    while (net->dirty_queue_count > 0 && tiles_sent_this_pass < tile_budget)
    {
        uint16_t tile_id = 0;

        if (!wd_dirty_queue_pop_locked(net, &tile_id, server->total_tiles))
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

        if (!wd_stream_send_tile_payload_locked(server, tile_id, next_generation, now, compressed_tile, compressed_size, &launched))
        {
            continue;
        }

        if (!launched)
        {
            wd_dirty_queue_reinsert_front_locked(net, tile_id, server->total_tiles);
            break;
        }

        memcpy(tile->compressed, compressed_tile, compressed_size);
        tile->last_hash       = hash;
        tile->generation      = next_generation;
        tile->timestamp_ns    = now;
        tile->compressed_size = compressed_size;

        net->stats.dirty_tiles++;
        tiles_sent_this_pass++;
    }

    /*
     * Keep scene_dirty true if there are queued tiles we could not send yet.
     * Otherwise, this pass fully drained known dirty work.
     */
    server->scene_dirty = net->dirty_queue_count > 0;

    pthread_mutex_unlock(&net->lock);

    wd_stream_policy_consume_tiles(server, tiles_sent_this_pass);

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

    pthread_mutex_unlock(&net->lock);

    /*
     * Keepalive generation summaries are expected while idle. Suppress the
     * stats line when they are the only activity so lossy-link repair stays
     * enabled without spamming logs.
     */
    bool useful_activity = s.dirty_tiles != 0 || s.udp_tiles_sent != 0 || s.udp_packets_sent != 0 || s.udp_bytes_sent != 0 ||
                           s.tcp_hello_rx != 0 || s.tcp_config_tx != 0 || s.retx_req_rx != 0 || s.retx_tiles_req != 0 ||
                           s.key_events_rx != 0 || s.key_events_injected != 0 || s.key_events_dropped != 0 || s.pointer_events_rx != 0 ||
                           s.pointer_events_injected != 0 || s.pointer_events_dropped != 0;

    if (!useful_activity)
    {
        return;
    }

    WD_LOG_DEBUG("WayDisplay stats/s: dirty_tiles=%llu udp_tiles_sent=%llu udp_pkts=%llu udp_kib=%.1f "
                 "tcp_hello_rx=%llu tcp_cfg_tx=%llu tcp_summary_tx=%llu retx_req_rx=%llu "
                 "retx_tiles_req=%llu "
                 "key_rx=%llu key_injected=%llu key_dropped=%llu pointer_rx=%llu pointer_injected=%llu "
                 "pointer_dropped=%llu input_net_avg_ms=%.2f input_queue_avg_ms=%.2f "
                 "input_to_summary_avg_ms=%.2f",
                 (unsigned long long)s.dirty_tiles, (unsigned long long)s.udp_tiles_sent, (unsigned long long)s.udp_packets_sent,
                 (double)s.udp_bytes_sent / 1024.0, (unsigned long long)s.tcp_hello_rx, (unsigned long long)s.tcp_config_tx,
                 (unsigned long long)s.tcp_summary_tx, (unsigned long long)s.retx_req_rx, (unsigned long long)s.retx_tiles_req,
                 (unsigned long long)s.key_events_rx, (unsigned long long)s.key_events_injected, (unsigned long long)s.key_events_dropped,
                 (unsigned long long)s.pointer_events_rx, (unsigned long long)s.pointer_events_injected,
                 (unsigned long long)s.pointer_events_dropped, wd_avg_ms(s.input_net_latency_sum_ns, s.input_net_latency_samples),
                 wd_avg_ms(s.input_queue_latency_sum_ns, s.input_queue_latency_samples),
                 wd_avg_ms(s.input_to_summary_sum_ns, s.input_to_summary_samples));
}
