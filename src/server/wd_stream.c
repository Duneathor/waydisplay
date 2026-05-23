#include "wd_server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "waydisplay/wd_net.h"
#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_time.h"
#include "waydisplay/wd_zstd.h"

#define WD_DEFAULT_PARTIAL_FPS 30u
#define WD_DEFAULT_LIMITED_TILES_PER_SECOND 120u
#define WD_MAX_REASONABLE_FPS 120u
#define WD_MAX_REASONABLE_TILES_PER_SECOND 10000u

void wd_stream_policy_set_defaults(struct wd_stream_policy *policy) {
    if (!policy) {
        return;
    }

    memset(policy, 0, sizeof(*policy));

    policy->mode = WD_STREAM_MODE_PARTIAL;
    policy->target_fps = WD_DEFAULT_PARTIAL_FPS;
    policy->max_tiles_per_second = WD_DEFAULT_LIMITED_TILES_PER_SECOND;
    policy->last_frame_send_ns = 0;
    policy->tile_tokens = 0.0;
    policy->last_token_refill_ns = 0;
}

void wd_stream_policy_apply_client_hello(
    struct wd_stream_policy *policy,
    const struct wd_client_hello_payload *hello) {
    if (!policy || !hello) {
        return;
    }

    uint16_t mode = hello->stream_mode;

    if (mode != WD_STREAM_MODE_FULL &&
        mode != WD_STREAM_MODE_PARTIAL &&
        mode != WD_STREAM_MODE_LIMITED) {
        mode = WD_STREAM_MODE_PARTIAL;
        }

        uint16_t fps = hello->target_fps;
    if (fps == 0) {
        fps = WD_DEFAULT_PARTIAL_FPS;
    }

    if (fps > WD_MAX_REASONABLE_FPS) {
        fps = WD_MAX_REASONABLE_FPS;
    }

    uint32_t max_tiles = hello->max_tiles_per_second;
    if (max_tiles == 0) {
        max_tiles = WD_DEFAULT_LIMITED_TILES_PER_SECOND;
    }

    if (max_tiles > WD_MAX_REASONABLE_TILES_PER_SECOND) {
        max_tiles = WD_MAX_REASONABLE_TILES_PER_SECOND;
    }

    policy->mode = mode;
    policy->target_fps = fps;
    policy->max_tiles_per_second = max_tiles;
    policy->last_frame_send_ns = 0;
    policy->tile_tokens = 0.0;
    policy->last_token_refill_ns = 0;
    }

    bool wd_stream_policy_should_render_now(struct wd_server *server, uint64_t now_ns) {
        if (!server) {
            return false;
        }

        struct wd_net_state *net = &server->net;

        pthread_mutex_lock(&net->lock);

        bool full_frame_needed = net->full_frame_needed;
        bool client_connected = net->client_connected;
        struct wd_stream_policy *policy = &net->stream_policy;

        if (!client_connected) {
            pthread_mutex_unlock(&net->lock);
            return false;
        }

        if (!server->scene_dirty && !full_frame_needed) {
            pthread_mutex_unlock(&net->lock);
            return false;
        }

        bool should = false;

        switch (policy->mode) {
            case WD_STREAM_MODE_FULL:
                should = true;
                break;

            case WD_STREAM_MODE_PARTIAL: {
                uint16_t fps = policy->target_fps ? policy->target_fps
                : WD_DEFAULT_PARTIAL_FPS;

                uint64_t interval_ns = 1000000000ull / fps;

                if (policy->last_frame_send_ns == 0 ||
                    now_ns - policy->last_frame_send_ns >= interval_ns ||
                    full_frame_needed) {
                    policy->last_frame_send_ns = now_ns;
                should = true;
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

                    if (policy->last_frame_send_ns == 0 ||
                        now_ns - policy->last_frame_send_ns >= interval_ns ||
                        full_frame_needed) {
                        policy->last_frame_send_ns = now_ns;
                    should = true;
                        }

                        break;
                }
        }

        pthread_mutex_unlock(&net->lock);

        return should;
    }

    uint32_t wd_stream_policy_tile_budget(struct wd_server *server, uint64_t now_ns) {
        struct wd_net_state *net = &server->net;

        pthread_mutex_lock(&net->lock);

        struct wd_stream_policy *policy = &net->stream_policy;

        if (policy->mode != WD_STREAM_MODE_LIMITED) {
            pthread_mutex_unlock(&net->lock);
            return WD_TOTAL_TILES;
        }

        if (policy->last_token_refill_ns == 0) {
            policy->last_token_refill_ns = now_ns;
            policy->tile_tokens = (double)policy->max_tiles_per_second;
        } else {
            uint64_t elapsed_ns = now_ns - policy->last_token_refill_ns;
            double elapsed_seconds = (double)elapsed_ns / 1000000000.0;

            policy->tile_tokens +=
            elapsed_seconds * (double)policy->max_tiles_per_second;

            /*
             * Allow one second of burst.
             */
            if (policy->tile_tokens > (double)policy->max_tiles_per_second) {
                policy->tile_tokens = (double)policy->max_tiles_per_second;
            }

            policy->last_token_refill_ns = now_ns;
        }

        uint32_t budget = (uint32_t)policy->tile_tokens;

        pthread_mutex_unlock(&net->lock);

        return budget;
    }

    void wd_stream_policy_consume_tiles(struct wd_server *server, uint32_t count) {
        struct wd_net_state *net = &server->net;

        pthread_mutex_lock(&net->lock);

        struct wd_stream_policy *policy = &net->stream_policy;

        if (policy->mode == WD_STREAM_MODE_LIMITED) {
            if (policy->tile_tokens >= (double)count) {
                policy->tile_tokens -= (double)count;
            } else {
                policy->tile_tokens = 0.0;
            }
        }

        pthread_mutex_unlock(&net->lock);
    }

bool wd_stream_init(struct wd_server *server) {
    const size_t compressed_capacity =
    wd_zstd_compress_bound(WD_UNCOMPRESSED_TILE_BYTES);

    for (uint16_t i = 0; i < WD_TOTAL_TILES; ++i) {
        struct wd_cached_tile *tile = &server->net.tiles[i];

        memset(tile, 0, sizeof(*tile));

        tile->compressed_capacity = (uint32_t)compressed_capacity;
        tile->compressed = malloc(compressed_capacity);

        if (!tile->compressed) {
            return false;
        }
    }

    return true;
}

void wd_stream_destroy(struct wd_server *server) {
    for (uint16_t i = 0; i < WD_TOTAL_TILES; ++i) {
        free(server->net.tiles[i].compressed);
        server->net.tiles[i].compressed = NULL;
        server->net.tiles[i].compressed_size = 0;
        server->net.tiles[i].compressed_capacity = 0;
    }
}

bool wd_stream_send_cached_tile_locked(struct wd_server *server, uint16_t tile_id) {
    struct wd_net_state *net = &server->net;

    if (tile_id >= WD_TOTAL_TILES) {
        return false;
    }

    struct wd_cached_tile *cache = &net->tiles[tile_id];

    if (!net->client_connected ||
        cache->compressed_size == 0 ||
        !cache->compressed) {
        return true;
        }

        uint16_t udp_payload_target = net->udp_payload_target;

    if (udp_payload_target == 0) {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target > 65487) {
        udp_payload_target = 65487;
    }

    if (udp_payload_target < 512) {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

        uint16_t packet_count =
        (uint16_t)((cache->compressed_size + udp_payload_target - 1) /
        udp_payload_target);

    for (uint16_t packet_id = 0; packet_id < packet_count; ++packet_id) {
        uint32_t offset = (uint32_t)packet_id * udp_payload_target;

        uint16_t payload_size =
        (uint16_t)(((cache->compressed_size - offset) > udp_payload_target)
        ? udp_payload_target
        : (cache->compressed_size - offset));

        uint8_t packet[sizeof(struct wd_udp_tile_packet_header) + 65487];

        struct wd_udp_tile_packet_header *h =
        (struct wd_udp_tile_packet_header *)packet;

        h->tile_id = tile_id;
        h->tile_pkt_count = packet_count;
        h->tile_pkt_id = packet_id;
        h->payload_size = payload_size;
        h->tile_generation = cache->generation;
        h->compressed_tile_size = cache->compressed_size;

        memcpy(packet + sizeof(*h),
               cache->compressed + offset,
               payload_size);

        ssize_t sent =
        sendto(net->udp_fd,
               packet,
               sizeof(*h) + payload_size,
               0,
               (const struct sockaddr *)&net->client_udp_addr,
               sizeof(net->client_udp_addr));

        if (sent < 0) {
            wlr_log(WLR_ERROR,
                    "WayDisplay: sendto failed: %s",
                    strerror(errno));
            return false;
        }

        net->stats.udp_packets_sent++;
        net->stats.udp_bytes_sent += (uint64_t)sent;
    }

    net->stats.udp_tiles_sent++;

    return true;
}

static bool wd_dirty_queue_push_locked(struct wd_net_state *net,
                                       uint16_t tile_id) {
    if (!net || tile_id >= WD_TOTAL_TILES) {
        return false;
    }

    if (net->dirty_queued[tile_id]) {
        return true;
    }

    if (net->dirty_queue_count >= WD_TOTAL_TILES) {
        /*
         * Queue is full. This should not happen because each tile can only
         * appear once, but keep it safe.
         */
        return false;
    }

    net->dirty_queue[net->dirty_queue_write] = tile_id;
    net->dirty_queue_write =
    (uint16_t)((net->dirty_queue_write + 1) % WD_TOTAL_TILES);
    net->dirty_queue_count++;
    net->dirty_queued[tile_id] = true;

    return true;
                                       }

                                       static bool wd_dirty_queue_pop_locked(struct wd_net_state *net,
                                                                             uint16_t *out_tile_id) {
                                           if (!net || !out_tile_id || net->dirty_queue_count == 0) {
                                               return false;
                                           }

                                           uint16_t tile_id = net->dirty_queue[net->dirty_queue_read];

                                           net->dirty_queue_read =
                                           (uint16_t)((net->dirty_queue_read + 1) % WD_TOTAL_TILES);
                                           net->dirty_queue_count--;

                                           if (tile_id < WD_TOTAL_TILES) {
                                               net->dirty_queued[tile_id] = false;
                                           }

                                           *out_tile_id = tile_id;
                                           return true;
                                                                             }

                                                                             static void wd_detect_dirty_tiles_into_queue_locked(struct wd_server *server) {
                                                                                 struct wd_net_state *net = &server->net;

                                                                                 /*
                                                                                  * Hash the current framebuffer and enqueue each changed tile once.
                                                                                  * A tile that changes repeatedly before being sent remains queued once;
                                                                                  * when sent, we send the latest pixels.
                                                                                  */
                                                                                 for (uint16_t tile_id = 0; tile_id < WD_TOTAL_TILES; ++tile_id) {
                                                                                     uint32_t hash =
                                                                                     wd_fnv1a_tile_hash_xrgb8888(server->framebuffer_xrgb8888,
                                                                                                                 tile_id);

                                                                                     if (hash != net->tiles[tile_id].last_hash) {
                                                                                         wd_dirty_queue_push_locked(net, tile_id);
                                                                                     }
                                                                                 }
                                                                             }

                                                                             bool wd_stream_send_dirty_tiles(struct wd_server *server) {
                                                                                 struct wd_net_state *net = &server->net;

                                                                                 if (!server->framebuffer_xrgb8888) {
                                                                                     return false;
                                                                                 }

                                                                                 const uint64_t now = wd_now_ns();
                                                                                 uint32_t tile_budget = wd_stream_policy_tile_budget(server, now);
                                                                                 uint32_t tiles_sent_this_pass = 0;

                                                                                 if (tile_budget == 0) {
                                                                                     return true;
                                                                                 }

                                                                                 uint8_t tile_bytes[WD_UNCOMPRESSED_TILE_BYTES];

                                                                                 pthread_mutex_lock(&net->lock);

                                                                                 if (!net->client_connected) {
                                                                                     pthread_mutex_unlock(&net->lock);
                                                                                     return true;
                                                                                 }

                                                                                 /*
                                                                                  * Phase 1:
                                                                                  * Progressive full-frame catch-up for new/reconnected clients.
                                                                                  *
                                                                                  * This sends each tile once, advancing full_frame_next_tile across
                                                                                  * multiple limited-mode passes. It does not restart at tile 0.
                                                                                  */
                                                                                 if (net->full_frame_needed) {
                                                                                     while (net->full_frame_next_tile < WD_TOTAL_TILES &&
                                                                                         tiles_sent_this_pass < tile_budget) {
                                                                                         uint16_t tile_id = net->full_frame_next_tile++;
                                                                                     struct wd_cached_tile *tile = &net->tiles[tile_id];

                                                                                     uint32_t hash =
                                                                                     wd_fnv1a_tile_hash_xrgb8888(server->framebuffer_xrgb8888,
                                                                                                                 tile_id);

                                                                                     bool cache_valid = tile->compressed_size > 0 && tile->compressed;

                                                                                     if (!cache_valid || hash != tile->last_hash) {
                                                                                         if (!wd_extract_tile_xrgb8888(server->framebuffer_xrgb8888,
                                                                                             tile_id,
                                                                                             tile_bytes)) {
                                                                                             continue;
                                                                                             }

                                                                                             uint32_t compressed_size = 0;

                                                                                         if (!wd_zstd_compress(tile_bytes,
                                                                                             WD_UNCOMPRESSED_TILE_BYTES,
                                                                                             tile->compressed,
                                                                                             tile->compressed_capacity,
                                                                                             WD_ZSTD_LEVEL,
                                                                                             &compressed_size)) {
                                                                                             wlr_log(WLR_ERROR,
                                                                                                     "WayDisplay: zstd compression failed for full-frame tile %u",
                                                                                                     tile_id);
                                                                                             continue;
                                                                                             }

                                                                                             tile->last_hash = hash;
                                                                                             tile->generation++;
                                                                                             tile->timestamp_ns = now;
                                                                                             tile->compressed_size = compressed_size;

                                                                                             net->stats.dirty_tiles++;
                                                                                     }

                                                                                     wd_stream_send_cached_tile_locked(server, tile_id);
                                                                                     tiles_sent_this_pass++;
                                                                                         }

                                                                                         if (net->full_frame_next_tile >= WD_TOTAL_TILES) {
                                                                                             net->full_frame_needed = false;
                                                                                             net->full_frame_next_tile = 0;
                                                                                             net->dirty_scan_next_tile = 0;

                                                                                             /*
                                                                                              * After initial catch-up, clear stale queue state and force the
                                                                                              * next normal pass to detect current dirty tiles from hashes.
                                                                                              */
                                                                                             memset(net->dirty_queued, 0, sizeof(net->dirty_queued));
                                                                                             net->dirty_queue_read = 0;
                                                                                             net->dirty_queue_write = 0;
                                                                                             net->dirty_queue_count = 0;

                                                                                             server->scene_dirty = false;
                                                                                         } else {
                                                                                             server->scene_dirty = true;
                                                                                         }

                                                                                         pthread_mutex_unlock(&net->lock);

                                                                                         wd_stream_policy_consume_tiles(server, tiles_sent_this_pass);

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

                                                                                 while (net->dirty_queue_count > 0 &&
                                                                                     tiles_sent_this_pass < tile_budget) {
                                                                                     uint16_t tile_id = 0;

                                                                                 if (!wd_dirty_queue_pop_locked(net, &tile_id)) {
                                                                                     break;
                                                                                 }

                                                                                 if (tile_id >= WD_TOTAL_TILES) {
                                                                                     continue;
                                                                                 }

                                                                                 struct wd_cached_tile *tile = &net->tiles[tile_id];

                                                                                 uint32_t hash =
                                                                                 wd_fnv1a_tile_hash_xrgb8888(server->framebuffer_xrgb8888,
                                                                                                             tile_id);

                                                                                 /*
                                                                                  * It may have been queued earlier but become identical by now.
                                                                                  */
                                                                                 if (hash == tile->last_hash) {
                                                                                     continue;
                                                                                 }

                                                                                 if (!wd_extract_tile_xrgb8888(server->framebuffer_xrgb8888,
                                                                                     tile_id,
                                                                                     tile_bytes)) {
                                                                                     continue;
                                                                                     }

                                                                                     uint32_t compressed_size = 0;

                                                                                 if (!wd_zstd_compress(tile_bytes,
                                                                                     WD_UNCOMPRESSED_TILE_BYTES,
                                                                                     tile->compressed,
                                                                                     tile->compressed_capacity,
                                                                                     WD_ZSTD_LEVEL,
                                                                                     &compressed_size)) {
                                                                                     wlr_log(WLR_ERROR,
                                                                                             "WayDisplay: zstd compression failed for dirty tile %u",
                                                                                             tile_id);
                                                                                     continue;
                                                                                     }

                                                                                     tile->last_hash = hash;
                                                                                     tile->generation++;
                                                                                     tile->timestamp_ns = now;
                                                                                     tile->compressed_size = compressed_size;

                                                                                     net->stats.dirty_tiles++;

                                                                                     wd_stream_send_cached_tile_locked(server, tile_id);
                                                                                     tiles_sent_this_pass++;
                                                                                     }

                                                                                     /*
                                                                                      * Keep scene_dirty true if there are queued tiles we could not send yet.
                                                                                      * Otherwise, this pass fully drained known dirty work.
                                                                                      */
                                                                                     server->scene_dirty = net->dirty_queue_count > 0;

                                                                                     pthread_mutex_unlock(&net->lock);

                                                                                     wd_stream_policy_consume_tiles(server, tiles_sent_this_pass);

                                                                                     return true;
                                                                             }

bool wd_stream_send_generation_summary_locked(struct wd_server *server) {
    struct wd_net_state *net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0) {
        return true;
    }

    struct wd_tile_summary_payload_header header;

    header.session_id = net->session_id;
    header.server_timestamp_ns = wd_now_ns();
    header.tile_count = WD_TOTAL_TILES;
    header.reserved = 0;

    size_t payload_size =
    sizeof(header) +
    WD_TOTAL_TILES * sizeof(struct wd_tile_generation_entry);

    uint8_t *payload = malloc(payload_size);
    if (!payload) {
        return false;
    }

    memcpy(payload, &header, sizeof(header));

    struct wd_tile_generation_entry *entries =
    (struct wd_tile_generation_entry *)(payload + sizeof(header));

    for (uint16_t i = 0; i < WD_TOTAL_TILES; ++i) {
        entries[i].tile_id = i;
        entries[i].reserved = 0;
        entries[i].tile_generation = net->tiles[i].generation;
        entries[i].tile_timestamp_ns = net->tiles[i].timestamp_ns;
    }

    bool ok =
    wd_send_tcp_message(net->tcp_fd,
                        WD_MSG_TILE_GENERATION_SUMMARY,
                        payload,
                        (uint32_t)payload_size);

    free(payload);

    if (ok) {
        net->stats.tcp_summary_tx++;
    }

    return ok;
}

void wd_stream_print_and_reset_stats(struct wd_server *server) {
    struct wd_net_state *net = &server->net;

    pthread_mutex_lock(&net->lock);

    struct wd_stats s = net->stats;
    memset(&net->stats, 0, sizeof(net->stats));

    pthread_mutex_unlock(&net->lock);

    wlr_log(WLR_INFO,
            "WayDisplay stats/s: dirty_tiles=%llu udp_tiles_sent=%llu udp_pkts=%llu udp_kib=%.1f "
            "tcp_hello_rx=%llu tcp_cfg_tx=%llu tcp_summary_tx=%llu retx_req_rx=%llu retx_tiles_req=%llu "
            "key_rx=%llu key_injected=%llu key_dropped=%llu",
            (unsigned long long)s.dirty_tiles,
            (unsigned long long)s.udp_tiles_sent,
            (unsigned long long)s.udp_packets_sent,
            (double)s.udp_bytes_sent / 1024.0,
            (unsigned long long)s.tcp_hello_rx,
            (unsigned long long)s.tcp_config_tx,
            (unsigned long long)s.tcp_summary_tx,
            (unsigned long long)s.retx_req_rx,
            (unsigned long long)s.retx_tiles_req,
            (unsigned long long)s.key_events_rx,
            (unsigned long long)s.key_events_injected,
            (unsigned long long)s.key_events_dropped);
}
