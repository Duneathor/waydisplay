#define _GNU_SOURCE

#include "waydisplay/wd_audio_transport.h"
#include "waydisplay/wd_input.h"
#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol_dispatch.h"
#include "waydisplay/wd_time.h"
#include "wd_async_tcp.h"
#include "wd_async_udp.h"
#include "wd_audio_stream.h"
#include "wd_connection_identity.h"
#include "wd_dirty_region_scheduler.h"
#include "wd_server_internal.h"
#include "wd_video_encoder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char* wd_video_mode_name(uint8_t mode) {
    switch (mode)
    {
    case WD_VIDEO_MODE_AUTO:
        return "auto";
    case WD_VIDEO_MODE_OFF:
        return "off";
    case WD_VIDEO_MODE_FORCE:
        return "force";
    default:
        return "unknown";
    }
}

static const char* wd_video_codec_name(uint32_t codec) {
    switch (codec)
    {
    case WD_VIDEO_CODEC_H264:
        return "h264";
    case WD_VIDEO_CODEC_H265:
        return "h265";
    default:
        return "none";
    }
}

static void wd_format_sockaddr_in(const struct sockaddr_in* addr, char* buf, size_t buf_size) {
    char ip[INET_ADDRSTRLEN];

    if (!addr || !buf || buf_size == 0)
    {
        return;
    }

    if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)))
    {
        snprintf(buf, buf_size, "<invalid>:%u", (unsigned)ntohs(addr->sin_port));
        return;
    }

    snprintf(buf, buf_size, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

static void wd_format_socket_endpoint(int fd, bool peer, char* buf, size_t buf_size) {
    struct sockaddr_in addr;
    socklen_t          addr_len = sizeof(addr);

    if (!buf || buf_size == 0)
    {
        return;
    }

    snprintf(buf, buf_size, "unavailable");

    if (fd < 0)
    {
        snprintf(buf, buf_size, "closed");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    if ((peer ? getpeername(fd, (struct sockaddr*)&addr, &addr_len) : getsockname(fd, (struct sockaddr*)&addr, &addr_len)) != 0)
    {
        snprintf(buf, buf_size, "unavailable:%s", strerror(errno));
        return;
    }

    if (addr.sin_family != AF_INET)
    {
        snprintf(buf, buf_size, "non-ipv4");
        return;
    }

    wd_format_sockaddr_in(&addr, buf, buf_size);
}

static void wd_log_tcp_channel_endpoint(const char* channel, int fd) {
    char local[64];
    char remote[64];

    wd_format_socket_endpoint(fd, false, local, sizeof(local));
    wd_format_socket_endpoint(fd, true, remote, sizeof(remote));
    WD_LOG_INFO("%s TCP channel connected local=%s remote=%s", channel, local, remote);
}

static uint64_t wd_clamp_u64(uint64_t value, uint64_t min_value, uint64_t max_value) {
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

static uint16_t wd_ns_to_ms_ceil_u16(uint64_t ns) {
    uint64_t ms = (ns + 999999ull) / 1000000ull;
    if (ms > UINT16_MAX)
    {
        return UINT16_MAX;
    }
    return (uint16_t)ms;
}

static void wd_net_set_link_profile_defaults(struct wd_net_state* net) {
    if (!net)
    {
        return;
    }

    net->link_rtt_ns                  = WD_LINK_RTT_DEFAULT_NS;
    net->link_jitter_ns               = 0;
    net->summary_retransmit_grace_ns  = WD_LINK_SUMMARY_GRACE_DEFAULT_NS;
    net->retransmit_request_interval_ns      = WD_LINK_RETRANSMIT_REQUEST_INTERVAL_DEFAULT_NS;
    net->retransmit_inflight_grace_ns = WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS;
    net->tile_reassembly_timeout_ns   = WD_LINK_TILE_REASSEMBLY_DEFAULT_NS;
    net->active_summary_interval_ns   = WD_LINK_ACTIVE_SUMMARY_INTERVAL_DEFAULT_NS;
    net->clean_summary_interval_ns    = WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS;
}

static uint64_t wd_net_estimate_summary_frame_bytes(const struct wd_server* server) {
    if (!server || server->total_tiles == 0)
    {
        return WD_TCP_HEADER_WIRE_SIZE + sizeof(struct wd_tile_summary_payload_header);
    }

    return (uint64_t)WD_TCP_HEADER_WIRE_SIZE + (uint64_t)sizeof(struct wd_tile_summary_payload_header) +
           (uint64_t)server->total_tiles * (uint64_t)sizeof(struct wd_tile_generation_entry);
}

static uint64_t wd_net_summary_budget_interval_ns(const struct wd_server* server, uint64_t base_interval_ns) {
    if (!server)
    {
        return base_interval_ns;
    }

    const struct wd_net_state* net  = &server->net;
    uint64_t                   rate = net->stream_policy.udp_rate_bytes_per_second;
    if (rate == 0)
    {
        return base_interval_ns;
    }

    uint64_t summary_budget_bytes_per_second = (rate * (uint64_t)WD_LINK_SUMMARY_BUDGET_PERCENT) / 100ull;
    if (summary_budget_bytes_per_second == 0)
    {
        return WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS;
    }

    uint64_t summary_frame_bytes = wd_net_estimate_summary_frame_bytes(server);
    uint64_t interval_ns =
        (summary_frame_bytes * WD_NSEC_PER_SEC + summary_budget_bytes_per_second - 1ull) / summary_budget_bytes_per_second;

    if (interval_ns < base_interval_ns)
    {
        interval_ns = base_interval_ns;
    }

    return wd_clamp_u64(interval_ns, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS);
}

static void wd_net_update_summary_cadence_for_budget(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net         = &server->net;
    uint64_t             base_active = wd_clamp_u64(net->link_rtt_ns / WD_LINK_PROFILE_ACTIVE_SUMMARY_RTT_DIVISOR,
                                                    WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS);
    uint64_t             active      = wd_net_summary_budget_interval_ns(server, base_active);
    uint64_t base_clean = wd_clamp_u64(net->link_rtt_ns / WD_LINK_PROFILE_CLEAN_SUMMARY_RTT_DIVISOR, WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS,
                                       WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS);
    uint64_t clean      = base_clean;
    if (active > clean / WD_LINK_PROFILE_CLEAN_TO_ACTIVE_MULTIPLIER)
    {
        clean = active * WD_LINK_PROFILE_CLEAN_TO_ACTIVE_MULTIPLIER;
    }

    net->active_summary_interval_ns = wd_clamp_u64(active, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS);
    net->clean_summary_interval_ns  = wd_clamp_u64(clean, WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS, WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS);
}

static void wd_net_derive_link_profile(struct wd_net_state* net, uint64_t measured_rtt_ns, uint64_t measured_jitter_ns) {
    if (!net)
    {
        return;
    }

    const uint64_t rtt_ns =
        wd_clamp_u64(measured_rtt_ns ? measured_rtt_ns : WD_LINK_RTT_DEFAULT_NS, WD_LINK_RTT_MIN_NS, WD_LINK_RTT_MAX_NS);
    const uint64_t jitter_ns = wd_clamp_u64(measured_jitter_ns, 0, WD_LINK_RTT_MAX_NS / 2ull);

    net->link_rtt_ns    = rtt_ns;
    net->link_jitter_ns = jitter_ns;

    net->summary_retransmit_grace_ns =
        wd_clamp_u64(rtt_ns + WD_LINK_PROFILE_JITTER_MULTIPLIER * jitter_ns + WD_LINK_PROFILE_SUMMARY_MARGIN_NS,
                     WD_LINK_SUMMARY_GRACE_MIN_NS, WD_LINK_SUMMARY_GRACE_MAX_NS);
    net->retransmit_request_interval_ns =
        wd_clamp_u64(rtt_ns + WD_LINK_PROFILE_JITTER_MULTIPLIER * jitter_ns + WD_LINK_PROFILE_RETRANSMIT_MARGIN_NS,
                     WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MIN_NS, WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MAX_NS);
    net->retransmit_inflight_grace_ns =
        wd_clamp_u64(rtt_ns + WD_LINK_PROFILE_JITTER_MULTIPLIER * jitter_ns + WD_LINK_PROFILE_RETRANSMIT_MARGIN_NS,
                     WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS, WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS);
    net->tile_reassembly_timeout_ns = wd_clamp_u64(rtt_ns / WD_LINK_PROFILE_REASSEMBLY_RTT_DIVISOR +
                                                       WD_LINK_PROFILE_JITTER_MULTIPLIER * jitter_ns + WD_LINK_PROFILE_REASSEMBLY_MARGIN_NS,
                                                   WD_LINK_TILE_REASSEMBLY_DEFAULT_NS, WD_LINK_TILE_REASSEMBLY_MAX_NS);
    net->active_summary_interval_ns = wd_clamp_u64(rtt_ns / WD_LINK_PROFILE_ACTIVE_SUMMARY_RTT_DIVISOR,
                                                   WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS, WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS);
    net->clean_summary_interval_ns = wd_clamp_u64(rtt_ns / WD_LINK_PROFILE_CLEAN_SUMMARY_RTT_DIVISOR, WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS,
                                                  WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS);
}

static void wd_close_fd(int* fd) {
    if (fd && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

static void wd_set_socket_timeout_ms(int fd, int optname, long timeout_ms) {
    struct timeval tv;

    memset(&tv, 0, sizeof(tv));

    if (timeout_ms > 0)
    {
        tv.tv_sec  = timeout_ms / 1000L;
        tv.tv_usec = (timeout_ms % 1000L) * 1000L;
    }

    (void)setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
}

static void wd_configure_accepted_tcp_socket(int tcp_fd) {
    int yes = 1;

    (void)setsockopt(tcp_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    (void)setsockopt(tcp_fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));

    wd_set_socket_timeout_ms(tcp_fd, SO_RCVTIMEO, WD_TCP_HANDSHAKE_TIMEOUT_MS);
    wd_set_socket_timeout_ms(tcp_fd, SO_SNDTIMEO, WD_TCP_CONNECTED_SEND_TIMEOUT_MS);
}

static void wd_clear_tcp_receive_timeout(int tcp_fd) {
    wd_set_socket_timeout_ms(tcp_fd, SO_RCVTIMEO, 0);
}

static void wd_net_publish_startup_result(struct wd_net_state* net, enum wd_net_startup_state state,
                                          enum wd_net_listener_stage failed_stage, int error_code) {
    pthread_mutex_lock(&net->lock);
    net->startup_state        = state;
    net->startup_failed_stage = failed_stage;
    net->startup_error        = error_code;
    if (state == WD_NET_STARTUP_FAILED)
    {
        wd_net_run_state_set(&net->run_state, false);
        pthread_cond_broadcast(&net->display_resize_cond);
    }
    pthread_cond_broadcast(&net->startup_cond);
    pthread_mutex_unlock(&net->lock);
}

bool wd_net_wait_until_ready(struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    pthread_mutex_lock(&net->lock);
    while (net->startup_state == WD_NET_STARTUP_PENDING)
    {
        pthread_cond_wait(&net->startup_cond, &net->lock);
    }
    const bool ready = net->startup_state == WD_NET_STARTUP_READY;
    if (!ready)
    {
        WD_LOG_ERROR("network startup failed during %s: %s", wd_net_listener_stage_name(net->startup_failed_stage),
                     strerror(net->startup_error));
    }
    pthread_mutex_unlock(&net->lock);
    return ready;
}

bool wd_net_init(struct wd_server* server, uint16_t tcp_port, struct in_addr listen_address) {
    struct wd_net_state* net = &server->net;

    memset(net, 0, sizeof(*net));

    if (pthread_mutex_init(&net->lock, NULL) != 0)
    {
        return false;
    }

    if (pthread_cond_init(&net->display_resize_cond, NULL) != 0)
    {
        pthread_mutex_destroy(&net->lock);
        return false;
    }

    if (pthread_cond_init(&net->startup_cond, NULL) != 0)
    {
        pthread_cond_destroy(&net->display_resize_cond);
        pthread_mutex_destroy(&net->lock);
        return false;
    }

    if (pthread_cond_init(&net->encoder_idle_cond, NULL) != 0)
    {
        pthread_cond_destroy(&net->startup_cond);
        pthread_cond_destroy(&net->display_resize_cond);
        pthread_mutex_destroy(&net->lock);
        return false;
    }

    if (pthread_mutex_init(&net->video_encoder_lock, NULL) != 0)
    {
        pthread_cond_destroy(&net->encoder_idle_cond);
        pthread_cond_destroy(&net->startup_cond);
        pthread_cond_destroy(&net->display_resize_cond);
        pthread_mutex_destroy(&net->lock);
        return false;
    }

    wd_net_run_state_init(&net->run_state, true);
    net->startup_state                   = WD_NET_STARTUP_PENDING;
    net->startup_failed_stage            = WD_NET_LISTENER_STAGE_NONE;
    net->startup_error                   = 0;
    net->listen_address                  = listen_address;
    net->tcp_port                        = tcp_port;
    net->tcp_fd                          = -1;
    net->input_tcp_fd                    = -1;
    net->selection_tcp_fd                = -1;
    net->video_tcp_fd                    = -1;
    net->audio_tcp_fd                    = -1;
    net->listen_fd                       = -1;
    net->control_tx                      = NULL;
    net->video_tx                        = NULL;
    net->udp_tx                          = NULL;
    net->video_encoder                   = NULL;
    net->audio_stream                    = NULL;
    net->udp_fd                          = -1;
    net->session_id                      = 0;
    net->connection_token                = 0;
    net->udp_port                        = 0;
    net->dirty_region_cursor             = 0;
    net->dirty_regions                   = calloc(server->total_tiles, sizeof(*net->dirty_regions));
    net->dirty_region_queued             = calloc(server->total_tiles, sizeof(*net->dirty_region_queued));
    net->dirty_region_enqueued_ns        = calloc(server->total_tiles, sizeof(*net->dirty_region_enqueued_ns));
    net->dirty_region_count              = 0;
    net->dirty_epochs                    = calloc(server->total_tiles, sizeof(*net->dirty_epochs));
    net->dirty_queue                     = calloc(server->total_tiles, sizeof(*net->dirty_queue));
    net->dirty_queued                    = calloc(server->total_tiles, sizeof(*net->dirty_queued));
    net->dirty_queue_enqueued_ns         = calloc(server->total_tiles, sizeof(*net->dirty_queue_enqueued_ns));
    net->retransmit_queue                = calloc(server->total_tiles, sizeof(*net->retransmit_queue));
    net->retransmit_queued               = calloc(server->total_tiles, sizeof(*net->retransmit_queued));
    net->retransmit_queue_enqueued_ns    = calloc(server->total_tiles, sizeof(*net->retransmit_queue_enqueued_ns));
    net->retransmit_requested_generation = calloc(server->total_tiles, sizeof(*net->retransmit_requested_generation));
    net->summary_dirty_tiles             = calloc(server->total_tiles, sizeof(*net->summary_dirty_tiles));
    net->summary_dirty_queue             = calloc(server->total_tiles, sizeof(*net->summary_dirty_queue));
    const bool async_transport_ready =
        wd_async_tcp_sender_create(&net->control_tx, WD_SERVER_CONTROL_TX_RING_ENTRIES) &&
        wd_async_tcp_sender_create(&net->video_tx, WD_SERVER_VIDEO_TX_RING_ENTRIES) &&
        wd_async_udp_sender_create(&net->udp_tx, WD_SERVER_UDP_TX_RING_ENTRIES);
    if (async_transport_ready)
    {
        wd_async_tcp_sender_set_max_pending_bytes(net->control_tx, WD_SERVER_CONTROL_TX_PENDING_BYTES);
        wd_async_tcp_sender_set_max_pending_bytes(net->video_tx, WD_SERVER_VIDEO_TX_PENDING_BYTES);
    }
    if (!wd_video_encoder_create(&net->video_encoder, server->video_encoder_backend))
    {
        net->video_encoder = NULL;
    }
    if (!wd_audio_stream_create(&net->audio_stream))
    {
        net->audio_stream = NULL;
    }
    if (!net->dirty_regions || !net->dirty_region_queued || !net->dirty_region_enqueued_ns || !net->dirty_epochs || !net->dirty_queue ||
        !net->dirty_queued || !net->dirty_queue_enqueued_ns || !net->retransmit_queue || !net->retransmit_queued ||
        !net->retransmit_queue_enqueued_ns || !net->retransmit_requested_generation || !net->summary_dirty_tiles ||
        !net->summary_dirty_queue || !async_transport_ready)
    {
        free(net->dirty_regions);
        free(net->dirty_region_queued);
        free(net->dirty_region_enqueued_ns);
        free(net->dirty_epochs);
        free(net->dirty_queue);
        free(net->dirty_queued);
        free(net->dirty_queue_enqueued_ns);
        free(net->retransmit_queue);
        free(net->retransmit_queued);
        free(net->retransmit_queue_enqueued_ns);
        free(net->retransmit_requested_generation);
        free(net->summary_dirty_tiles);
        free(net->summary_dirty_queue);
        wd_async_tcp_sender_destroy(net->control_tx);
        wd_async_tcp_sender_destroy(net->video_tx);
        wd_async_udp_sender_destroy(net->udp_tx);
        wd_video_encoder_destroy(net->video_encoder);
        wd_audio_stream_destroy(net->audio_stream);
        pthread_mutex_destroy(&net->video_encoder_lock);
        net->control_tx                      = NULL;
        net->video_tx                        = NULL;
        net->udp_tx                          = NULL;
        net->video_encoder                   = NULL;
        net->audio_stream                    = NULL;
        net->dirty_regions                   = NULL;
        net->dirty_region_queued             = NULL;
        net->dirty_region_enqueued_ns        = NULL;
        net->dirty_region_count              = 0;
        net->dirty_epochs                    = NULL;
        net->dirty_queue                     = NULL;
        net->dirty_queued                    = NULL;
        net->dirty_queue_enqueued_ns         = NULL;
        net->retransmit_queue                = NULL;
        net->retransmit_queued               = NULL;
        net->retransmit_queue_enqueued_ns    = NULL;
        net->retransmit_requested_generation = NULL;
        net->summary_dirty_tiles             = NULL;
        net->summary_dirty_queue             = NULL;
        pthread_cond_destroy(&net->encoder_idle_cond);
        pthread_cond_destroy(&net->startup_cond);
        pthread_cond_destroy(&net->display_resize_cond);
        pthread_mutex_destroy(&net->lock);
        return false;
    }
    net->dirty_region_count     = 0;
    net->dirty_queue_read       = 0;
    net->dirty_queue_write      = 0;
    net->dirty_queue_count      = 0;
    net->retransmit_queue_count = 0;
    net->summary_dirty_count    = 0;
    net->udp_payload_target     = WD_UDP_PAYLOAD_TARGET;
    wd_net_set_link_profile_defaults(net);

    wd_stream_policy_set_defaults(&net->stream_policy);

    return true;
}

void wd_net_destroy(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    wd_net_run_state_set(&net->run_state, false);

    /* Stop encoder and tile workers before destroying their senders, codec,
     * mutexes, or frame storage. */
    wd_stream_destroy(server);

    if (net->listen_fd >= 0)
    {
        close(net->listen_fd);
        net->listen_fd = -1;
    }

    if (net->tcp_fd >= 0)
    {
        close(net->tcp_fd);
        net->tcp_fd = -1;
    }
    if (net->input_tcp_fd >= 0)
    {
        close(net->input_tcp_fd);
        net->input_tcp_fd = -1;
    }
    if (net->selection_tcp_fd >= 0)
    {
        close(net->selection_tcp_fd);
        net->selection_tcp_fd = -1;
    }
    wd_audio_stream_stop(net->audio_stream);
    if (net->video_tcp_fd >= 0)
    {
        close(net->video_tcp_fd);
        net->video_tcp_fd = -1;
    }
    if (net->audio_tcp_fd >= 0)
    {
        close(net->audio_tcp_fd);
        net->audio_tcp_fd = -1;
    }

    if (net->udp_fd >= 0)
    {
        close(net->udp_fd);
        net->udp_fd = -1;
    }

    wd_async_tcp_sender_destroy(net->control_tx);
    wd_async_tcp_sender_destroy(net->video_tx);
    wd_async_udp_sender_destroy(net->udp_tx);
    wd_video_encoder_destroy(net->video_encoder);
    wd_audio_stream_destroy(net->audio_stream);
    pthread_mutex_destroy(&net->video_encoder_lock);
    net->control_tx    = NULL;
    net->video_tx      = NULL;
    net->udp_tx        = NULL;
    net->video_encoder = NULL;
    net->audio_stream  = NULL;

    wd_dirty_region_scheduler_destroy(net->dirty_region_scheduler);
    net->dirty_region_scheduler = NULL;

    free(net->dirty_regions);
    net->dirty_regions = NULL;
    free(net->dirty_region_queued);
    net->dirty_region_queued = NULL;
    free(net->dirty_region_enqueued_ns);
    net->dirty_region_enqueued_ns = NULL;
    free(net->dirty_epochs);
    net->dirty_epochs       = NULL;
    net->dirty_region_count = 0;
    free(net->dirty_queue);
    net->dirty_queue = NULL;

    free(net->dirty_queued);
    net->dirty_queued = NULL;

    free(net->dirty_queue_enqueued_ns);
    net->dirty_queue_enqueued_ns = NULL;

    free(net->retransmit_queue);
    net->retransmit_queue = NULL;

    free(net->retransmit_queued);
    net->retransmit_queued = NULL;

    free(net->retransmit_queue_enqueued_ns);
    net->retransmit_queue_enqueued_ns = NULL;

    free(net->retransmit_requested_generation);
    net->retransmit_requested_generation = NULL;

    free(net->summary_dirty_tiles);
    net->summary_dirty_tiles = NULL;

    free(net->summary_dirty_queue);
    net->summary_dirty_queue = NULL;

    net->summary_dirty_count = 0;

    free(net->clipboard_text);
    net->clipboard_text         = NULL;
    net->clipboard_text_size    = 0;
    net->clipboard_text_pending = false;

    free(net->primary_text);
    net->primary_text              = NULL;
    net->primary_text_size         = 0;
    net->primary_text_pending      = false;
    net->clipboard_request_pending = false;
    net->primary_request_pending   = false;

    pthread_cond_destroy(&net->encoder_idle_cond);
    pthread_cond_destroy(&net->startup_cond);
    pthread_cond_destroy(&net->display_resize_cond);
    pthread_mutex_destroy(&net->lock);
}

static bool wd_udp_socket_set_pmtu_mode(int udp_fd, int mode) {
#if defined(IP_MTU_DISCOVER)
    return setsockopt(udp_fd, IPPROTO_IP, IP_MTU_DISCOVER, &mode, sizeof(mode)) == 0;
#else
    (void)udp_fd;
    (void)mode;
    return false;
#endif
}

static void wd_udp_socket_disable_df_best_effort(int udp_fd) {
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DONT)
    /*
     * The MTU probe deliberately forces DF, but the data stream should not
     * inherit that policy. The payload target is still selected to fit a
     * normal Ethernet/Wi-Fi MTU, but explicitly allowing fragmentation here
     * avoids black-holing steady-state tile packets if probe state or host
     * defaults leave PMTU discovery in a strict DF mode.
     */
    (void)wd_udp_socket_set_pmtu_mode(udp_fd, IP_PMTUDISC_DONT);
#else
    (void)udp_fd;
#endif
}

static int wd_create_udp_mtu_probe_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -1;
    }

#if defined(IP_MTU_DISCOVER)
#if defined(IP_PMTUDISC_PROBE)
    const int mode = IP_PMTUDISC_PROBE;
#elif defined(IP_PMTUDISC_DO)
    const int mode = IP_PMTUDISC_DO;
#else
    const int mode = -1;
#endif

    if (mode < 0 || !wd_udp_socket_set_pmtu_mode(fd, mode))
    {
        close(fd);
        return -1;
    }
#else
    close(fd);
    return -1;
#endif

    return fd;
}

static uint16_t run_udp_mtu_probe(struct wd_server* server, int tcp_fd, const struct sockaddr_in* client_udp_addr) {
    struct wd_net_state* net       = &server->net;
    uint8_t              tile_size = 0;
    if (!wd_tile_size_code_for_dimensions(server->tile_width, server->tile_height, &tile_size))
    {
        WD_LOG_ERROR("cannot probe UDP with unsupported tile geometry %ux%u", server->tile_width, server->tile_height);
        return WD_UDP_PAYLOAD_TARGET;
    }

    /*
     * Payload size excluding wd_udp_tile_packet_header.
     *
     * Probe sizes are tile payload bytes, excluding
     * struct wd_udp_tile_packet_header. Include exact payload budgets for
     * common IPv4 MTUs so the selected value is the real maximum usable
     * tile payload for that path instead of the next lower coarse bucket.
     */
    static const uint16_t probe_sizes[] = {
        WD_UDP_TILE_PAYLOAD_MAX,
        WD_IPV4_MTU_TO_TILE_PAYLOAD(WD_NET_MTU_PROBE_JUMBO_BYTES),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(WD_NET_MTU_PROBE_LARGE_BYTES),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(WD_NET_MTU_PROBE_MEDIUM_BYTES),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(WD_NET_MTU_PROBE_ETHERNET_BYTES),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(WD_NET_MTU_PROBE_PPPOE_BYTES),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(WD_NET_MTU_PROBE_TUNNEL_BYTES),
        WD_NET_MTU_PROBE_PAYLOAD_HIGH,
        WD_NET_MTU_PROBE_PAYLOAD_MEDIUM,
        WD_NET_MTU_PROBE_PAYLOAD_LOW,
        WD_NET_MTU_PROBE_PAYLOAD_FLOOR,
    };

    const uint16_t probe_count = (uint16_t)(sizeof(probe_sizes) / sizeof(probe_sizes[0]));

    struct wd_mtu_probe_start_payload start;
    memset(&start, 0, sizeof(start));
    start.session_id       = net->session_id;
    start.connection_token = net->connection_token;
    start.probe_count      = probe_count;

    if (!wd_send_tcp_message(tcp_fd, WD_MSG_MTU_PROBE_START, &start, sizeof(start)))
    {
        return WD_UDP_PAYLOAD_TARGET;
    }

    int probe_udp_fd = wd_create_udp_mtu_probe_socket();
    if (probe_udp_fd < 0)
    {
        WD_LOG_INFO("UDP MTU probe could not force DF; using default UDP payload target: %u", WD_UDP_PAYLOAD_TARGET);
        wd_udp_socket_disable_df_best_effort(net->udp_fd);
        return WD_UDP_PAYLOAD_TARGET;
    }

    /*
     * Give the client a moment to enter its UDP-probe receive loop.
     * Probe packets are sent with IPv4 DF set so successful receipt means
     * the datagram fit the path MTU instead of being IP-fragmented and
     * reassembled.
     */
    wd_sleep_ms(WD_NET_PROBE_STARTUP_DELAY_MS);

    uint8_t packet[WD_UDP_TILE_HEADER_MAX_SIZE + WD_UDP_TILE_PAYLOAD_MAX];

    for (uint16_t i = 0; i < probe_count; ++i)
    {
        uint16_t     payload_size = probe_sizes[i];
        const size_t packet_size  = WD_UDP_TILE_HEADER_MIN_SIZE + payload_size;

        memset(packet, 0, sizeof(packet));
        struct wd_udp_tile_packet_decoded h;
        memset(&h, 0, sizeof(h));
        h.session_id        = net->session_id;
        h.connection_token  = net->connection_token;
        h.content_epoch     = net->content_epoch;
        h.flags             = WD_UDP_TILE_FLAG_COMPRESSED;
        h.tile_size         = tile_size;
        h.tile_id           = WD_UDP_TILE_ID_MTU_PROBE;
        h.tile_pkt_count    = probe_count > UINT8_MAX ? UINT8_MAX : (uint8_t)probe_count;
        h.tile_pkt_id       = i > UINT8_MAX ? UINT8_MAX : (uint8_t)i;
        h.payload_size      = payload_size;
        h.tile_payload_size = payload_size;
        h.tile_generation   = net->session_id;
        if (!wd_udp_tile_packet_encode_header(packet, sizeof(packet), &h))
        {
            continue;
        }

        memset(packet + WD_UDP_TILE_HEADER_MIN_SIZE, 0xa5, payload_size);

        ssize_t sent = sendto(probe_udp_fd, packet, packet_size, 0, (const struct sockaddr*)client_udp_addr, sizeof(*client_udp_addr));

        if (sent < 0 || (size_t)sent != packet_size)
        {
            /*
             * EMSGSIZE is expected for probes above the path/interface MTU
             * when DF is set. Keep trying smaller probes.
             */
            continue;
        }
    }

    close(probe_udp_fd);
    probe_udp_fd = -1;
    wd_udp_socket_disable_df_best_effort(net->udp_fd);

    uint16_t type         = 0;
    uint8_t* payload      = NULL;
    uint32_t payload_size = 0;

    if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size))
    {
        free(payload);
        return WD_UDP_PAYLOAD_TARGET;
    }

    uint16_t result = WD_UDP_PAYLOAD_TARGET;

    if (wd_protocol_message_allowed(type, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION,
                                    WD_PROTOCOL_CLIENT_TO_SERVER, payload_size) &&
        type == WD_MSG_MTU_PROBE_RESULT)
    {
        struct wd_mtu_probe_result_payload probe_result;
        memcpy(&probe_result, payload, sizeof(probe_result));

        if (probe_result.session_id == net->session_id && probe_result.connection_token == net->connection_token &&
            probe_result.max_udp_payload_received >= WD_MIN_PROBED_UDP_PAYLOAD)
        {
            result = probe_result.max_udp_payload_received;
        }
    }

    free(payload);

    if (result > WD_UDP_TILE_PAYLOAD_MAX)
    {
        result = WD_UDP_TILE_PAYLOAD_MAX;
    }

    if (result < WD_MIN_PROBED_UDP_PAYLOAD)
    {
        result = WD_UDP_PAYLOAD_TARGET;
    }

    WD_LOG_INFO("UDP payload target selected by probe: %u", result);

    return result;
}

static uint64_t run_udp_throughput_probe(struct wd_server* server, int tcp_fd, const struct sockaddr_in* client_udp_addr,
                                         uint16_t udp_payload_target) {
    struct wd_net_state* net       = &server->net;
    uint8_t              tile_size = 0;
    if (!wd_tile_size_code_for_dimensions(server->tile_width, server->tile_height, &tile_size))
    {
        WD_LOG_ERROR("cannot probe throughput with unsupported tile geometry %ux%u", server->tile_width, server->tile_height);
        return WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }

    if (udp_payload_target == 0 || udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target < WD_MIN_PROBED_UDP_PAYLOAD)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    size_t packet_size = WD_UDP_TILE_HEADER_MIN_SIZE + udp_payload_target;

    struct wd_throughput_probe_start_payload start;
    memset(&start, 0, sizeof(start));
    start.session_id       = net->session_id;
    start.connection_token = net->connection_token;
    /*
     * UINT8_MAX means the probe is duration-limited. Earlier code sent a
     * fixed WD_THROUGHPUT_PROBE_TARGET_BYTES budget paced across the probe
     * window, which capped the best possible localhost result to roughly
     * 8 MiB / 750 ms * safety ~= 9 MiB/s. Keep the UDP tile probe header in
     * its normal uint8_t packet-count shape, but repeat packet ids and let the
     * client count every received probe datagram until the deadline.
     */
    start.probe_count  = UINT8_MAX;
    start.payload_size = udp_payload_target;
    start.duration_ms  = WD_THROUGHPUT_PROBE_DURATION_MS;

    if (!wd_send_tcp_message(tcp_fd, WD_MSG_THROUGHPUT_PROBE_START, &start, sizeof(start)))
    {
        return WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }

    wd_udp_socket_disable_df_best_effort(net->udp_fd);

    /* Give the client a moment to switch from MTU probing to throughput probing. */
    wd_sleep_ms(WD_NET_PROBE_STARTUP_DELAY_MS);

    uint8_t* packet = malloc(packet_size);
    if (!packet)
    {
        return WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }

    memset(packet, 0, packet_size);
    struct wd_udp_tile_packet_decoded h;
    memset(&h, 0, sizeof(h));
    h.session_id        = net->session_id;
    h.connection_token  = net->connection_token;
    h.content_epoch     = net->content_epoch;
    h.flags             = WD_UDP_TILE_FLAG_COMPRESSED;
    h.tile_size         = tile_size;
    h.tile_id           = WD_UDP_TILE_ID_THROUGHPUT_PROBE;
    h.tile_pkt_count    = (uint8_t)start.probe_count;
    h.payload_size      = udp_payload_target;
    h.tile_payload_size = udp_payload_target;
    h.tile_generation   = net->session_id;
    memset(packet + WD_UDP_TILE_HEADER_MIN_SIZE, 0x5a, udp_payload_target);

    const uint64_t start_ns    = wd_now_ns();
    const uint64_t duration_ns = (uint64_t)WD_THROUGHPUT_PROBE_DURATION_MS * WD_NSEC_PER_MSEC;
    const uint64_t deadline_ns = start_ns + duration_ns;

    uint32_t packets_sent = 0;
    while (wd_now_ns() < deadline_ns)
    {
        h.tile_pkt_id = (uint8_t)(packets_sent % UINT8_MAX);
        if (!wd_udp_tile_packet_encode_header(packet, packet_size, &h))
        {
            break;
        }

        ssize_t sent = sendto(net->udp_fd, packet, packet_size, 0, (const struct sockaddr*)client_udp_addr, sizeof(*client_udp_addr));
        if (sent == (ssize_t)packet_size)
        {
            packets_sent++;
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == ENOBUFS))
        {
            wd_sleep_ms(WD_NET_PROBE_RETRY_SLEEP_MS);
            continue;
        }

        if (sent < 0 && errno == EINTR)
        {
            continue;
        }

        /* Other errors are unexpected during the throughput probe; stop
         * sending and use whatever the client received. */
        break;
    }

    free(packet);

    uint16_t type            = 0;
    uint8_t* payload         = NULL;
    uint32_t payload_size_rx = 0;

    if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size_rx))
    {
        free(payload);
        return WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    }

    uint64_t udp_rate     = WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND;
    uint32_t packets_received = 0;

    if (wd_protocol_message_allowed(type, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION,
                                    WD_PROTOCOL_CLIENT_TO_SERVER, payload_size_rx) &&
        type == WD_MSG_THROUGHPUT_PROBE_RESULT)
    {
        struct wd_throughput_probe_result_payload result;
        memcpy(&result, payload, sizeof(result));
        packets_received = result.packets_received;

        if (result.session_id == net->session_id && result.connection_token == net->connection_token && result.bytes_received > 0 &&
            result.duration_ms > 0)
        {
            uint64_t bytes_per_second = ((uint64_t)result.bytes_received * 1000ull) / result.duration_ms;
            bytes_per_second          = (bytes_per_second * WD_UDP_THROUGHPUT_SAFETY_PERCENT) / 100ull;

            if (bytes_per_second < WD_UDP_RATE_MIN_BYTES_PER_SECOND)
            {
                bytes_per_second = WD_UDP_RATE_MIN_BYTES_PER_SECOND;
            }
            if (bytes_per_second > WD_UDP_RATE_MAX_BYTES_PER_SECOND)
            {
                bytes_per_second = WD_UDP_RATE_MAX_BYTES_PER_SECOND;
            }

            udp_rate = bytes_per_second;
        }
    }

    free(payload);

    WD_LOG_INFO("adaptive UDP byte budget selected by throughput probe: %llu KiB/s sent=%u recv=%u",
                (unsigned long long)(udp_rate / 1024ull), packets_sent, packets_received);

    return udp_rate;
}

static void run_tcp_link_probe(struct wd_server* server, int tcp_fd) {
    struct wd_net_state* net = &server->net;
    enum { WD_LINK_PROBE_COUNT = WD_NET_LINK_PROBE_COUNT };

    uint64_t samples[WD_LINK_PROBE_COUNT];
    uint32_t sample_count = 0;

    for (uint32_t i = 0; i < WD_LINK_PROBE_COUNT; ++i)
    {
        struct wd_link_probe_payload ping;
        memset(&ping, 0, sizeof(ping));
        ping.session_id       = net->session_id;
        ping.connection_token = net->connection_token;
        ping.sequence         = i + 1u;
        ping.timestamp_ns     = wd_now_ns();

        if (!wd_send_tcp_message(tcp_fd, WD_MSG_LINK_PROBE_PING, &ping, sizeof(ping)))
        {
            break;
        }

        uint16_t type         = 0;
        uint8_t* payload      = NULL;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size))
        {
            free(payload);
            break;
        }

        if (wd_protocol_message_allowed(type, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION,
                                        WD_PROTOCOL_CLIENT_TO_SERVER, payload_size) &&
            type == WD_MSG_LINK_PROBE_PONG)
        {
            struct wd_link_probe_payload pong;
            memcpy(&pong, payload, sizeof(pong));
            if (pong.session_id == net->session_id && pong.connection_token == net->connection_token && pong.sequence == ping.sequence)
            {
                uint64_t now_ns = wd_now_ns();
                if (now_ns >= ping.timestamp_ns)
                {
                    samples[sample_count++] = now_ns - ping.timestamp_ns;
                }
            }
        }

        free(payload);

        if (sample_count == 0 && i >= 1)
        {
            break;
        }
    }

    if (sample_count == 0)
    {
        wd_net_derive_link_profile(net, WD_LINK_RTT_DEFAULT_NS, 0);
        wd_net_update_summary_cadence_for_budget(server);
        WD_LOG_INFO("TCP link RTT probe unavailable; using conservative defaults rtt=%u ms summary_delta=%llu/%llu ms",
                    (unsigned)(WD_LINK_RTT_DEFAULT_NS / 1000000ull), (unsigned long long)(net->active_summary_interval_ns / 1000000ull),
                    (unsigned long long)(net->clean_summary_interval_ns / 1000000ull));
        return;
    }

    uint64_t sum_ns = 0;
    uint64_t max_ns = 0;
    uint64_t min_ns = UINT64_MAX;
    for (uint32_t i = 0; i < sample_count; ++i)
    {
        sum_ns += samples[i];
        if (samples[i] > max_ns)
        {
            max_ns = samples[i];
        }
        if (samples[i] < min_ns)
        {
            min_ns = samples[i];
        }
    }

    uint64_t avg_ns = sum_ns / sample_count;
    /* Use the average, not the minimum, and include half the sample spread so
     * very fast LAN measurements do not collapse timers to overly aggressive
     * values while high-latency/jittery links get more slack. */
    uint64_t jitter_ns = (max_ns > min_ns) ? (max_ns - min_ns) / 2ull : 0;
    wd_net_derive_link_profile(net, avg_ns, jitter_ns);
    wd_net_update_summary_cadence_for_budget(server);

    WD_LOG_INFO("TCP link profile rtt=%llu ms jitter=%llu ms summary_grace=%llu ms request_interval=%llu ms reassembly=%llu ms "
                "summary_delta=%llu/%llu ms samples=%u",
                (unsigned long long)(net->link_rtt_ns / 1000000ull), (unsigned long long)(net->link_jitter_ns / 1000000ull),
                (unsigned long long)(net->summary_retransmit_grace_ns / 1000000ull),
                (unsigned long long)(net->retransmit_request_interval_ns / 1000000ull),
                (unsigned long long)(net->tile_reassembly_timeout_ns / 1000000ull),
                (unsigned long long)(net->active_summary_interval_ns / 1000000ull),
                (unsigned long long)(net->clean_summary_interval_ns / 1000000ull), sample_count);
}

static void wd_server_fill_config(struct wd_server* server, uint8_t session_id, uint16_t udp_payload_target,
                                  struct wd_server_config_payload* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    wd_net_update_summary_cadence_for_budget(server);

    cfg->session_id         = session_id;
    cfg->connection_token   = server->net.connection_token;
    cfg->config_epoch       = server->net.config_epoch;
    cfg->content_epoch      = server->net.content_epoch;
    cfg->media_clock_id     = server->net.media_clock_id;
    cfg->server_udp_port    = server->net.udp_port;
    cfg->width              = (uint16_t)server->display_width;
    cfg->height             = (uint16_t)server->display_height;
    cfg->tile_width         = server->tile_width;
    cfg->tile_height        = server->tile_height;
    cfg->tiles_x            = server->tiles_x;
    cfg->tiles_y            = server->tiles_y;
    cfg->total_tiles        = server->total_tiles;
    cfg->pixel_format       = WD_PIXEL_FORMAT_XRGB8888;
    cfg->compression_mode   = WD_COMPRESSION_ZSTD;
    cfg->zstd_level         = WD_ZSTD_LEVEL;
    cfg->udp_payload_target = udp_payload_target;
    cfg->capabilities       = 0;
    if (server->net.video_stream_negotiated)
    {
        cfg->capabilities |= WD_SERVER_CAP_VIDEO_STREAM;
        cfg->video_codecs    = server->net.video_codecs;
        cfg->video_transport = server->net.video_transport;
    }
    if (server->net.audio_stream_negotiated)
    {
        cfg->capabilities |= WD_SERVER_CAP_AUDIO_STREAM;
        cfg->audio_codec             = server->net.audio_codec;
        cfg->audio_transport         = server->net.audio_transport;
        cfg->audio_sample_rate       = WD_AUDIO_SAMPLE_RATE_DEFAULT;
        cfg->audio_channels          = server->net.audio_channels;
        cfg->audio_frame_samples     = WD_AUDIO_FRAME_SAMPLES_DEFAULT;
        cfg->audio_target_latency_ms = server->net.audio_target_latency_ms;
        cfg->audio_bitrate           = server->net.audio_bitrate;
    }
    cfg->link_rtt_ms                  = wd_ns_to_ms_ceil_u16(server->net.link_rtt_ns);
    cfg->summary_retransmit_grace_ms  = wd_ns_to_ms_ceil_u16(server->net.summary_retransmit_grace_ns);
    cfg->retransmit_request_interval_ms      = wd_ns_to_ms_ceil_u16(server->net.retransmit_request_interval_ns);
    cfg->retransmit_inflight_grace_ms = wd_ns_to_ms_ceil_u16(server->net.retransmit_inflight_grace_ns);
    cfg->tile_reassembly_timeout_ms   = wd_ns_to_ms_ceil_u16(server->net.tile_reassembly_timeout_ns);
    cfg->active_summary_interval_ms   = wd_ns_to_ms_ceil_u16(server->net.active_summary_interval_ns);
    cfg->clean_summary_interval_ms    = wd_ns_to_ms_ceil_u16(server->net.clean_summary_interval_ns);
}

bool wd_server_send_current_config_locked(struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    if (!net->client_connected || net->tcp_fd < 0 || net->session_id == 0)
    {
        return false;
    }

    struct wd_server_config_payload cfg;
    wd_server_fill_config(server, net->session_id, net->udp_payload_target, &cfg);

    (void)wd_async_tcp_sender_drop_message_type(net->control_tx, WD_MSG_SERVER_CONFIG);
    (void)wd_async_tcp_sender_drop_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY);
    if (wd_async_tcp_sender_has_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY))
    {
        net->stats.tcp_async_send_failed++;
        (void)shutdown(net->tcp_fd, SHUT_RDWR);
        return false;
    }
    const bool ok = wd_async_tcp_send_message(net->control_tx, net->tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg));
    if (!ok)
    {
        net->stats.tcp_async_send_failed++;
        (void)shutdown(net->tcp_fd, SHUT_RDWR);
    }

    if (ok)
    {
        wd_stream_account_tcp_control_bytes_locked(net, (uint32_t)(WD_TCP_HEADER_WIRE_SIZE + sizeof(cfg)));
        net->stats.tcp_config_tx++;

        /* Do not release the new-session tile stream merely because the
         * config was queued to TCP. UDP can overtake TCP, and the client must
         * rebuild its framebuffer/texture before it can accept the new
         * session. WD_MSG_CONFIG_APPLIED is the readiness barrier. */
        if (net->config_update_pending)
        {
            net->config_update_sent_ns = wd_now_ns();
        }
    }
    return ok;
}

static void wd_server_handle_keyboard_message(struct wd_server* server, const struct wd_server_config_payload* cfg, const uint8_t* payload,
                                              uint32_t payload_size) {
    struct wd_net_state* net = &server->net;

    if (payload_size != sizeof(struct wd_keyboard_event_payload))
    {
        return;
    }

    struct wd_keyboard_event_payload key;
    memcpy(&key, payload, sizeof(key));

    if (wd_keyboard_event_payload_is_valid(&key, payload_size) && key.session_id == cfg->session_id &&
        key.connection_token == cfg->connection_token)
    {
        pthread_mutex_lock(&net->lock);
        wd_keyboard_queue_event_locked(net, &key, wd_now_ns());
        pthread_mutex_unlock(&net->lock);
        wd_server_wake_input(server);
    }
}

static void wd_server_handle_pointer_message(struct wd_server* server, const struct wd_server_config_payload* cfg, const uint8_t* payload,
                                             uint32_t payload_size) {
    struct wd_net_state* net = &server->net;

    if (payload_size != sizeof(struct wd_pointer_event_payload))
    {
        return;
    }

    struct wd_pointer_event_payload pointer;
    memcpy(&pointer, payload, sizeof(pointer));

    if (wd_pointer_event_payload_is_valid(&pointer, payload_size) && pointer.session_id == cfg->session_id &&
        pointer.connection_token == cfg->connection_token)
    {
        if (pointer.event_type == WD_POINTER_EVENT_BUTTON && pointer.button == WD_INPUT_BUTTON_RIGHT)
        {
            WD_LOG_DEBUG("received right click %s from client "
                         "x=%u y=%u mods=0x%x timestamp=%" PRIu64,
                         pointer.button_state == WD_POINTER_BUTTON_PRESSED ? "press" : "release", pointer.x, pointer.y, pointer.modifiers,
                         pointer.client_timestamp_ns);
        }
        pthread_mutex_lock(&net->lock);
        wd_pointer_queue_event_locked(net, &pointer, wd_now_ns());
        pthread_mutex_unlock(&net->lock);
        wd_server_wake_input(server);
    }
}

static bool wd_accept_aux_channel_fd(struct wd_server* server, uint8_t session_id, uint64_t connection_token, int* input_tcp_fd,
                                     int* selection_tcp_fd, int* video_tcp_fd, int* audio_tcp_fd) {
    struct wd_net_state* net = &server->net;

    struct sockaddr_in peer_addr;
    socklen_t          peer_len = sizeof(peer_addr);
    int                fd       = accept4(net->listen_fd, (struct sockaddr*)&peer_addr, &peer_len, SOCK_CLOEXEC);

    if (fd < 0)
    {
        return false;
    }

    wd_configure_accepted_tcp_socket(fd);

    uint16_t type         = 0;
    uint8_t* payload      = NULL;
    uint32_t payload_size = 0;

    if (!wd_recv_tcp_message(fd, &type, &payload, &payload_size))
    {
        free(payload);
        close(fd);
        return false;
    }

    bool accepted = false;

    if (!wd_protocol_message_allowed(type, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
                                     WD_PROTOCOL_CLIENT_TO_SERVER, payload_size))
    {
        WD_LOG_WARN("rejected auxiliary handshake message=%s(%u) size=%u", wd_protocol_message_name(type), type, payload_size);
        free(payload);
        close(fd);
        return false;
    }

    if (type == WD_MSG_INPUT_CHANNEL_HELLO && payload_size == sizeof(struct wd_input_channel_hello_payload) && *input_tcp_fd < 0)
    {
        struct wd_input_channel_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));

        if (hello.session_id == session_id && hello.connection_token == connection_token)
        {
            *input_tcp_fd = fd;
            accepted      = true;
        }
    }
    else if (type == WD_MSG_SELECTION_CHANNEL_HELLO && payload_size == sizeof(struct wd_selection_channel_hello_payload) &&
             *selection_tcp_fd < 0)
    {
        struct wd_selection_channel_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));

        if (hello.session_id == session_id && hello.connection_token == connection_token)
        {
            *selection_tcp_fd = fd;
            accepted          = true;
        }
    }
    else if (type == WD_MSG_VIDEO_CHANNEL_HELLO && payload_size == sizeof(struct wd_video_channel_hello_payload) && video_tcp_fd &&
             *video_tcp_fd < 0)
    {
        struct wd_video_channel_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));

        if (hello.session_id == session_id && hello.connection_token == connection_token &&
            (hello.video_codecs & ~WD_VIDEO_CODEC_MASK) == 0 && net->video_stream_negotiated &&
            (hello.video_codecs & net->video_codecs & WD_VIDEO_CODEC_MASK) != 0 && hello.video_transport == net->video_transport)
        {
            *video_tcp_fd = fd;
            accepted      = true;
        }
    }
    else if (type == WD_MSG_AUDIO_CHANNEL_HELLO && payload_size == sizeof(struct wd_audio_channel_hello_payload) && audio_tcp_fd &&
             *audio_tcp_fd < 0)
    {
        struct wd_audio_channel_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));

        if (hello.session_id == session_id && hello.connection_token == connection_token && net->audio_stream_negotiated &&
            hello.audio_codecs == net->audio_codec && hello.audio_transport == net->audio_transport)
        {
            *audio_tcp_fd = fd;
            accepted      = true;
        }
    }

    free(payload);

    if (!accepted)
    {
        close(fd);
        return false;
    }

    wd_clear_tcp_receive_timeout(fd);
    return true;
}

static bool wd_accept_required_aux_channels(struct wd_server* server, uint8_t session_id, uint64_t connection_token, int* input_tcp_fd,
                                            int* selection_tcp_fd, int* video_tcp_fd, int* audio_tcp_fd) {
    struct wd_net_state* net         = &server->net;
    const uint64_t       deadline_ns = wd_now_ns() + WD_NET_AUX_CHANNEL_ACCEPT_TIMEOUT_NS;

    *input_tcp_fd     = -1;
    *selection_tcp_fd = -1;
    if (video_tcp_fd)
    {
        *video_tcp_fd = -1;
    }
    if (audio_tcp_fd)
    {
        *audio_tcp_fd = -1;
    }

    while ((*input_tcp_fd < 0 || *selection_tcp_fd < 0) && wd_now_ns() < deadline_ns)
    {
        uint64_t now_ns = wd_now_ns();
        if (now_ns >= deadline_ns)
        {
            break;
        }

        uint64_t remaining_ns = deadline_ns - now_ns;
        int      timeout_ms   = (int)(remaining_ns / WD_NSEC_PER_MSEC);
        if (timeout_ms <= 0)
        {
            timeout_ms = 1;
        }

        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd     = net->listen_fd;
        pfd.events = POLLIN;

        int poll_rc;
        do
        {
            poll_rc = poll(&pfd, 1, timeout_ms);
        } while (poll_rc < 0 && errno == EINTR);

        if (poll_rc <= 0 || !(pfd.revents & POLLIN))
        {
            break;
        }

        (void)wd_accept_aux_channel_fd(server, session_id, connection_token, input_tcp_fd, selection_tcp_fd, video_tcp_fd, audio_tcp_fd);
    }

    return *input_tcp_fd >= 0 && *selection_tcp_fd >= 0;
}

static int wd_tcp_reader_poll_timeout_ms(const struct wd_tcp_reader* const* readers, size_t reader_count, uint64_t now_ns) {
    uint64_t earliest_deadline_ns = 0;

    for (size_t i = 0; i < reader_count; ++i)
    {
        const struct wd_tcp_reader* reader = readers[i];
        if (!wd_tcp_reader_has_partial_frame(reader))
        {
            continue;
        }

        const uint64_t deadline_ns = wd_tcp_reader_deadline_ns(reader);
        if (deadline_ns != 0 && (earliest_deadline_ns == 0 || deadline_ns < earliest_deadline_ns))
        {
            earliest_deadline_ns = deadline_ns;
        }
    }

    if (earliest_deadline_ns == 0)
    {
        return -1;
    }

    if (earliest_deadline_ns <= now_ns)
    {
        return 0;
    }

    const uint64_t remaining_ns = earliest_deadline_ns - now_ns;
    const uint64_t timeout_ms   = (remaining_ns + WD_NSEC_PER_MSEC - 1u) / WD_NSEC_PER_MSEC;
    return timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms;
}

void* wd_net_thread_main(void* arg) {
    struct wd_server*    server = arg;
    struct wd_net_state* net    = &server->net;

    struct wd_net_listener     listener;
    enum wd_net_listener_stage failed_stage  = WD_NET_LISTENER_STAGE_NONE;
    int                        startup_error = 0;

    if (!wd_net_listener_open(&listener, net->tcp_port, &net->listen_address, &failed_stage, &startup_error))
    {
        wd_net_publish_startup_result(net, WD_NET_STARTUP_FAILED, failed_stage, startup_error);
        return NULL;
    }

    net->listen_fd = listener.listen_fd;
    net->udp_fd    = listener.udp_fd;
    net->tcp_port  = listener.tcp_port;
    net->udp_port  = listener.udp_port;

    wd_udp_socket_disable_df_best_effort(net->udp_fd);

    {
        char tcp_local[64];
        char udp_local[64];

        wd_format_socket_endpoint(net->listen_fd, false, tcp_local, sizeof(tcp_local));
        wd_format_socket_endpoint(net->udp_fd, false, udp_local, sizeof(udp_local));
        WD_LOG_INFO("network listening tcp=%s udp_sender=%s tcp_fd=%d udp_fd=%d", tcp_local, udp_local, net->listen_fd, net->udp_fd);
    }

    int sndbuf = WD_UDP_SOCKET_BUFFER_BYTES;
    if (setsockopt(net->udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
    {
        WD_LOG_ERROR("setsockopt SO_SNDBUF failed: %s", strerror(errno));
    }
    else
    {
        int       actual     = 0;
        socklen_t actual_len = sizeof(actual);

        if (getsockopt(net->udp_fd, SOL_SOCKET, SO_SNDBUF, &actual, &actual_len) == 0)
        {
            WD_LOG_INFO("UDP send buffer requested=%d actual=%d", sndbuf, actual);
        }
    }

    wd_net_publish_startup_result(net, WD_NET_STARTUP_READY, WD_NET_LISTENER_STAGE_NONE, 0);

    while (wd_net_run_state_is_running(&net->run_state))
    {
        struct sockaddr_in peer_addr;
        socklen_t          peer_len = sizeof(peer_addr);

        int tcp_fd = accept4(net->listen_fd, (struct sockaddr*)&peer_addr, &peer_len, SOCK_CLOEXEC);

        if (tcp_fd < 0)
        {
            if (!wd_net_run_state_is_running(&net->run_state))
            {
                break;
            }

            if (errno == EINTR)
            {
                continue;
            }

            WD_LOG_ERROR("accept failed: %s", strerror(errno));
            continue;
        }

        wd_configure_accepted_tcp_socket(tcp_fd);

        uint16_t type         = 0;
        uint8_t* payload      = NULL;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size) ||
            !wd_protocol_message_allowed(type, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, payload_size) ||
            type != WD_MSG_CLIENT_HELLO)
        {
            WD_LOG_ERROR("invalid client hello");
            free(payload);
            close(tcp_fd);
            continue;
        }

        struct wd_client_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));
        free(payload);
        payload = NULL;

        if (!wd_client_hello_payload_is_valid(&hello, sizeof(hello)) || hello.client_udp_port == 0)
        {
            WD_LOG_ERROR("rejected invalid client hello");
            close(tcp_fd);
            continue;
        }

        if (hello.desired_width != 0 || hello.desired_height != 0)
        {
            if (hello.desired_width == 0 || hello.desired_height == 0 ||
                !wd_server_request_display_size(server, hello.desired_width, hello.desired_height))
            {
                WD_LOG_ERROR("rejected requested client display size %ux%u", hello.desired_width, hello.desired_height);
                close(tcp_fd);
                continue;
            }
        }

        const uint32_t selected_video_codec =
            (hello.capabilities & WD_CLIENT_CAP_VIDEO_STREAM) != 0 && hello.video_transport == WD_VIDEO_TRANSPORT_TCP
                ? wd_video_encoder_choose_codec(net->video_encoder, hello.video_codecs)
                : 0;
        const bool client_video_tcp = selected_video_codec != 0;
        const bool client_audio_tcp = (hello.capabilities & WD_CLIENT_CAP_AUDIO_STREAM) != 0 &&
                                      (hello.audio_codecs & WD_AUDIO_CODEC_OPUS) != 0 && hello.audio_transport == WD_AUDIO_TRANSPORT_TCP &&
                                      hello.audio_max_channels >= 1 && net->audio_stream && wd_audio_stream_ready(net->audio_stream);
        const uint8_t  selected_audio_channels = client_audio_tcp && hello.audio_max_channels >= 2 ? 2 : (client_audio_tcp ? 1 : 0);
        const uint16_t selected_audio_latency_ms =
            client_audio_tcp && hello.audio_target_latency_ms != 0 ? hello.audio_target_latency_ms : WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;

        struct wd_server_config_payload cfg;
        uint8_t                         session_id = 0;

        uint64_t connection_token = 0;
        uint64_t media_clock_id   = 0;
        if (!wd_connection_identity_generate(&connection_token, &media_clock_id))
        {
            WD_LOG_ERROR("failed to obtain secure randomness for connection identity");
            close(tcp_fd);
            continue;
        }

        pthread_mutex_lock(&net->lock);

        net->connection_token     = connection_token;
        net->media_clock_id       = media_clock_id;
        net->media_clock_start_ns = wd_now_ns();
        net->connection_epoch++;
        if (net->connection_epoch == 0)
        {
            net->connection_epoch = 1;
        }
        net->config_epoch++;
        if (net->config_epoch == 0)
        {
            net->config_epoch = 1;
        }
        net->content_epoch++;
        net->input_correlation_inflight_sequence = 0;
        if (net->content_epoch == 0)
        {
            net->content_epoch = 1;
        }

        net->session_id = wd_connection_next_session_id(net->session_id);
        session_id      = net->session_id;

        pthread_mutex_unlock(&net->lock);

        struct sockaddr_in client_udp_addr;
        memset(&client_udp_addr, 0, sizeof(client_udp_addr));

        client_udp_addr.sin_family = AF_INET;
        client_udp_addr.sin_addr   = peer_addr.sin_addr;
        client_udp_addr.sin_port   = htons(hello.client_udp_port);

        uint16_t selected_udp_payload      = run_udp_mtu_probe(server, tcp_fd, &client_udp_addr);
        uint64_t selected_udp_rate = run_udp_throughput_probe(server, tcp_fd, &client_udp_addr, selected_udp_payload);

        pthread_mutex_lock(&net->lock);
        net->udp_payload_target      = selected_udp_payload;
        net->video_stream_negotiated = client_video_tcp;
        net->video_codecs            = client_video_tcp ? selected_video_codec : 0;
        net->video_transport         = client_video_tcp ? WD_VIDEO_TRANSPORT_TCP : 0;
        net->audio_stream_negotiated = client_audio_tcp;
        net->audio_codec             = client_audio_tcp ? WD_AUDIO_CODEC_OPUS : 0;
        net->audio_transport         = client_audio_tcp ? WD_AUDIO_TRANSPORT_TCP : 0;
        net->audio_channels          = selected_audio_channels;
        net->audio_target_latency_ms = client_audio_tcp ? selected_audio_latency_ms : 0;
        net->audio_bitrate           = client_audio_tcp ? WD_AUDIO_BITRATE_DEFAULT : 0;
        if (client_audio_tcp)
        {
            net->audio_epoch++;
            if (net->audio_epoch == 0)
            {
                net->audio_epoch = 1;
            }
        }
        const uint64_t tile_udp_rate = client_audio_tcp
                                           ? wd_audio_reserve_from_tile_budget(selected_udp_rate, WD_AUDIO_BITRATE_DEFAULT)
                                           : selected_udp_rate;
        wd_stream_policy_set_udp_rate(&net->stream_policy, tile_udp_rate);
        if (client_audio_tcp)
        {
            WD_LOG_INFO("audio bandwidth reservation: link=%llu KiB/s reserved=%llu KiB/s tile_budget=%llu KiB/s",
                        (unsigned long long)(selected_udp_rate / 1024ull),
                        (unsigned long long)(wd_audio_reserved_bytes_per_second(WD_AUDIO_BITRATE_DEFAULT) / 1024ull),
                        (unsigned long long)(tile_udp_rate / 1024ull));
        }
        wd_stream_policy_apply_client_hello(&net->stream_policy, &hello);
        WD_LOG_INFO(
            "video mode control: mode=%s bitrate_kib=%u min_dirty_pct=%u enter_seconds=%u exit_dirty_pct=%u exit_seconds=%u negotiated=%s",
            wd_video_mode_name(net->stream_policy.video_mode), net->stream_policy.video_bitrate_kib_per_second,
            net->stream_policy.video_min_dirty_percent, net->stream_policy.video_enter_seconds, net->stream_policy.video_exit_dirty_percent,
            net->stream_policy.video_exit_seconds, client_video_tcp ? "yes" : "no");
        pthread_mutex_unlock(&net->lock);

        if ((hello.capabilities & WD_CLIENT_CAP_VIDEO_STREAM) != 0 && hello.video_transport == WD_VIDEO_TRANSPORT_TCP && !client_video_tcp)
        {
            WD_LOG_INFO("video stream unavailable for requested codecs=0x%x; server codecs=0x%x backend=%s", hello.video_codecs,
                        wd_video_encoder_supported_codecs(net->video_encoder), wd_video_encoder_backend_name(net->video_encoder));
        }
        if ((hello.capabilities & WD_CLIENT_CAP_AUDIO_STREAM) != 0 && !client_audio_tcp)
        {
            WD_LOG_INFO("audio stream unavailable: capture=%s encoder=%s requested_codecs=0x%x transport=%u",
                        wd_audio_stream_capture_backend_name(), wd_audio_stream_encoder_backend_name(), hello.audio_codecs,
                        hello.audio_transport);
        }

        run_tcp_link_probe(server, tcp_fd);

        wd_server_fill_config(server, session_id, selected_udp_payload, &cfg);

        if (!wd_send_tcp_message(tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg)))
        {
            WD_LOG_ERROR("failed to send server config");
            close(tcp_fd);
            continue;
        }

        int input_tcp_fd     = -1;
        int selection_tcp_fd = -1;
        int video_tcp_fd     = -1;
        int audio_tcp_fd     = -1;
        if (!wd_accept_required_aux_channels(server, cfg.session_id, cfg.connection_token, &input_tcp_fd, &selection_tcp_fd,
                                             &video_tcp_fd, &audio_tcp_fd))
        {
            WD_LOG_ERROR("required input/selection channels were not established");
            wd_close_fd(&input_tcp_fd);
            wd_close_fd(&selection_tcp_fd);
            wd_close_fd(&video_tcp_fd);
            wd_close_fd(&audio_tcp_fd);
            close(tcp_fd);
            continue;
        }

        wd_clear_tcp_receive_timeout(tcp_fd);

        pthread_mutex_lock(&net->lock);

        net->tcp_fd           = tcp_fd;
        net->input_tcp_fd     = input_tcp_fd;
        net->selection_tcp_fd = selection_tcp_fd;
        net->video_tcp_fd     = video_tcp_fd;
        net->audio_tcp_fd     = audio_tcp_fd;
        net->stats.tcp_input_channel_accepted++;
        net->stats.tcp_selection_channel_accepted++;
        if (video_tcp_fd >= 0)
        {
            net->stats.tcp_video_channel_accepted++;
        }
        net->client_udp_addr       = client_udp_addr;
        net->client_connected      = true;
        net->config_update_pending = false;
        net->config_update_sent_ns = 0;
        if (net->control_tx)
        {
            (void)wd_async_tcp_sender_drop_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY);
        }
        net->summary_epoch++;
        if (net->summary_epoch == 0)
        {
            net->summary_epoch = 1;
        }
        net->dirty_region_cursor = 0;
        wd_dirty_region_scheduler_reset(net->dirty_region_scheduler);
        if (net->dirty_region_queued)
        {
            memset(net->dirty_region_queued, 0, server->total_tiles * sizeof(*net->dirty_region_queued));
        }
        if (net->dirty_region_enqueued_ns)
        {
            memset(net->dirty_region_enqueued_ns, 0, server->total_tiles * sizeof(*net->dirty_region_enqueued_ns));
        }
        net->dirty_region_count = 0;
        if (net->dirty_queued)
        {
            memset(net->dirty_queued, 0, server->total_tiles * sizeof(*net->dirty_queued));
        }
        if (net->dirty_queue_enqueued_ns)
        {
            memset(net->dirty_queue_enqueued_ns, 0, server->total_tiles * sizeof(*net->dirty_queue_enqueued_ns));
        }
        if (net->retransmit_queued)
        {
            memset(net->retransmit_queued, 0, server->total_tiles * sizeof(*net->retransmit_queued));
        }
        if (net->retransmit_queue_enqueued_ns)
        {
            memset(net->retransmit_queue_enqueued_ns, 0, server->total_tiles * sizeof(*net->retransmit_queue_enqueued_ns));
        }
        if (net->retransmit_requested_generation)
        {
            memset(net->retransmit_requested_generation, 0, server->total_tiles * sizeof(*net->retransmit_requested_generation));
        }
        if (net->summary_dirty_tiles)
        {
            memset(net->summary_dirty_tiles, 0, server->total_tiles * sizeof(*net->summary_dirty_tiles));
        }
        net->dirty_queue_read       = 0;
        net->dirty_queue_write      = 0;
        net->dirty_queue_count      = 0;
        net->retransmit_queue_count = 0;
        net->summary_dirty_count    = 0;
        wd_stream_invalidate_all_tiles_locked(server);
        wd_server_request_full_refresh(server);
        net->stream_policy.last_frame_send_ns              = 0;
        net->stream_policy.video_auto_bootstrap_suppressed = true;
        net->stream_policy.video_auto_bootstrap_seconds    = 0;
        server->last_summary_ns                            = 0;
        server->last_delta_summary_ns                      = 0;
        WD_LOG_DEBUG("waiting for compositor-owned full refresh for new client");

        net->stats.tcp_hello_rx++;
        net->stats.tcp_config_tx++;

        net->key_queue_count         = 0;
        net->pointer_queue_count     = 0;
        net->key_state_reset_pending = true;

        free(net->clipboard_text);
        net->clipboard_text         = NULL;
        net->clipboard_text_size    = 0;
        net->clipboard_text_pending = false;

        free(net->primary_text);
        net->primary_text              = NULL;
        net->primary_text_size         = 0;
        net->primary_text_pending      = false;
        net->clipboard_request_pending = false;
        net->primary_request_pending   = false;
        wd_cursor_queue_current_locked(server);

        pthread_mutex_unlock(&net->lock);
        wd_server_wake_input(server);

        if (audio_tcp_fd >= 0 && !wd_audio_stream_start(net->audio_stream, audio_tcp_fd, cfg.session_id, cfg.connection_token,
                                                        net->audio_epoch, cfg.media_clock_id, net->media_clock_start_ns,
                                                        selected_audio_channels, WD_AUDIO_BITRATE_DEFAULT, selected_audio_latency_ms))
        {
            WD_LOG_ERROR("failed to start negotiated audio stream");
            close(audio_tcp_fd);
            audio_tcp_fd = -1;
            pthread_mutex_lock(&net->lock);
            net->audio_tcp_fd = -1;
            pthread_mutex_unlock(&net->lock);
        }

        {
            char control_local[64];
            char control_remote[64];
            char udp_local[64];
            char udp_remote[64];

            wd_format_socket_endpoint(tcp_fd, false, control_local, sizeof(control_local));
            wd_format_socket_endpoint(tcp_fd, true, control_remote, sizeof(control_remote));
            wd_format_socket_endpoint(net->udp_fd, false, udp_local, sizeof(udp_local));
            wd_format_sockaddr_in(&client_udp_addr, udp_remote, sizeof(udp_remote));

            WD_LOG_INFO("client connected; control_tcp=%s<->%s udp=%s->%s input_channel=%s selection_channel=%s video_channel=%s "
                        "video_stream=%s video_codec=%s video_transport=%s audio_channel=%s audio_stream=%s audio_codec=%s display=%ux%u "
                        "tile=%ux%u requested_capture_fps=%u requested_udp_rate_cap_kib_per_sec=%u adaptive_udp_rate_kib_per_sec=%llu",
                        control_local, control_remote, udp_local, udp_remote, input_tcp_fd >= 0 ? "yes" : "no",
                        selection_tcp_fd >= 0 ? "yes" : "no", video_tcp_fd >= 0 ? "yes" : "no", client_video_tcp ? "yes" : "no",
                        client_video_tcp ? wd_video_codec_name(selected_video_codec) : "none", client_video_tcp ? "tcp" : "none",
                        audio_tcp_fd >= 0 ? "yes" : "no", client_audio_tcp ? "yes" : "no", client_audio_tcp ? "opus" : "none",
                        server->display_width, server->display_height, server->tile_width, server->tile_height, hello.requested_capture_fps,
                        hello.udp_rate_cap_kib_per_second, (unsigned long long)(net->stream_policy.udp_rate_bytes_per_second / 1024ull));

            if (input_tcp_fd >= 0)
            {
                wd_log_tcp_channel_endpoint("input", input_tcp_fd);
            }
            if (selection_tcp_fd >= 0)
            {
                wd_log_tcp_channel_endpoint("selection", selection_tcp_fd);
            }
            if (video_tcp_fd >= 0)
            {
                wd_log_tcp_channel_endpoint("video", video_tcp_fd);
            }
            if (audio_tcp_fd >= 0)
            {
                wd_log_tcp_channel_endpoint("audio", audio_tcp_fd);
            }
        }

        struct wd_tcp_reader control_reader;
        struct wd_tcp_reader input_reader;
        struct wd_tcp_reader selection_reader;
        struct wd_tcp_reader video_reader;
        wd_tcp_reader_init(&control_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_CONTROL,
                                                                             WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                             WD_PROTOCOL_CLIENT_TO_SERVER));
        wd_tcp_reader_init(&input_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_INPUT,
                                                                           WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                           WD_PROTOCOL_CLIENT_TO_SERVER));
        wd_tcp_reader_init(&selection_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_SELECTION,
                                                                               WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                               WD_PROTOCOL_CLIENT_TO_SERVER));
        wd_tcp_reader_init(&video_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_VIDEO,
                                                                           WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                           WD_PROTOCOL_CLIENT_TO_SERVER));

        while (wd_net_run_state_is_running(&net->run_state))
        {
            struct pollfd pfds[6];
            nfds_t        nfds            = 0;
            nfds_t        control_pfd_idx = 0;
            nfds_t        input_pfd_idx   = 0;
            nfds_t        select_pfd_idx  = 0;
            nfds_t        video_pfd_idx   = 0;
            nfds_t        audio_pfd_idx   = 0;
            nfds_t        listen_pfd_idx  = 0;
            bool          have_input_pfd  = false;
            bool          have_select_pfd = false;
            bool          have_video_pfd  = false;
            bool          have_audio_pfd  = false;
            bool          have_listen_pfd = false;

            memset(pfds, 0, sizeof(pfds));
            control_pfd_idx   = nfds;
            pfds[nfds].fd     = tcp_fd;
            pfds[nfds].events = POLLIN;
            nfds++;

            if (input_tcp_fd >= 0)
            {
                input_pfd_idx     = nfds;
                have_input_pfd    = true;
                pfds[nfds].fd     = input_tcp_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }

            if (selection_tcp_fd >= 0)
            {
                select_pfd_idx    = nfds;
                have_select_pfd   = true;
                pfds[nfds].fd     = selection_tcp_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }

            if (video_tcp_fd >= 0)
            {
                video_pfd_idx     = nfds;
                have_video_pfd    = true;
                pfds[nfds].fd     = video_tcp_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }
            if (audio_tcp_fd >= 0)
            {
                audio_pfd_idx     = nfds;
                have_audio_pfd    = true;
                pfds[nfds].fd     = audio_tcp_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }

            if ((client_video_tcp && video_tcp_fd < 0) || (client_audio_tcp && audio_tcp_fd < 0))
            {
                listen_pfd_idx    = nfds;
                have_listen_pfd   = true;
                pfds[nfds].fd     = net->listen_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }

            const struct wd_tcp_reader* readers[] = {&control_reader, &input_reader, &selection_reader, &video_reader};
            const int poll_timeout_ms = wd_tcp_reader_poll_timeout_ms(readers, sizeof(readers) / sizeof(readers[0]), wd_now_ns());

            int poll_rc;
            do
            {
                poll_rc = poll(pfds, nfds, poll_timeout_ms);
            } while (poll_rc < 0 && errno == EINTR);

            if (poll_rc < 0)
            {
                break;
            }

            const uint64_t receive_now_ns = wd_now_ns();

            if (have_listen_pfd && (pfds[listen_pfd_idx].revents & POLLIN))
            {
                int old_video_fd = video_tcp_fd;
                int old_audio_fd     = audio_tcp_fd;

                (void)wd_accept_aux_channel_fd(server, cfg.session_id, cfg.connection_token, &input_tcp_fd, &selection_tcp_fd,
                                               &video_tcp_fd, &audio_tcp_fd);

                if (video_tcp_fd >= 0 && old_video_fd < 0)
                {
                    pthread_mutex_lock(&net->lock);
                    net->video_tcp_fd = video_tcp_fd;
                    net->stats.tcp_video_channel_accepted++;
                    pthread_mutex_unlock(&net->lock);
                    wd_tcp_reader_reset(&video_reader);
                    wd_log_tcp_channel_endpoint("late video", video_tcp_fd);
                }

                if (audio_tcp_fd >= 0 && old_audio_fd < 0)
                {
                    pthread_mutex_lock(&net->lock);
                    net->audio_tcp_fd = audio_tcp_fd;
                    pthread_mutex_unlock(&net->lock);
                    if (!wd_audio_stream_start(net->audio_stream, audio_tcp_fd, cfg.session_id, cfg.connection_token, net->audio_epoch,
                                               cfg.media_clock_id, net->media_clock_start_ns, selected_audio_channels,
                                               WD_AUDIO_BITRATE_DEFAULT, selected_audio_latency_ms))
                    {
                        WD_LOG_ERROR("failed to start late audio channel");
                        close(audio_tcp_fd);
                        audio_tcp_fd = -1;
                        pthread_mutex_lock(&net->lock);
                        net->audio_tcp_fd = -1;
                        pthread_mutex_unlock(&net->lock);
                    }
                    else
                    {
                        wd_log_tcp_channel_endpoint("late audio", audio_tcp_fd);
                    }
                }
            }

            if (have_input_pfd && ((pfds[input_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) ||
                                        wd_tcp_reader_has_partial_frame(&input_reader)))
            {
                struct wd_tcp_message input_message;
                enum wd_tcp_reader_status input_status =
                    wd_tcp_reader_receive(&input_reader, input_tcp_fd, receive_now_ns, WD_TCP_FRAME_TIMEOUT_NS, &input_message);

                if (input_status == WD_TCP_READER_MESSAGE &&
                    !wd_protocol_message_allowed(input_message.message_type, WD_PROTOCOL_CHANNEL_INPUT,
                                                 WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
                                                 input_message.payload_size))
                {
                    WD_LOG_WARN("rejected input channel message=%s(%u) size=%u", wd_protocol_message_name(input_message.message_type),
                                input_message.message_type, input_message.payload_size);
                    wd_tcp_message_release(&input_message);
                    input_status = WD_TCP_READER_INVALID_FRAME;
                }

                if (input_status == WD_TCP_READER_MESSAGE)
                {
                    pthread_mutex_lock(&net->lock);
                    net->stats.tcp_input_channel_rx++;
                    pthread_mutex_unlock(&net->lock);

                    if (input_message.message_type == WD_MSG_KEYBOARD_KEY)
                    {
                        wd_server_handle_keyboard_message(server, &cfg, input_message.payload, input_message.payload_size);
                    }
                    else if (input_message.message_type == WD_MSG_POINTER_EVENT)
                    {
                        wd_server_handle_pointer_message(server, &cfg, input_message.payload, input_message.payload_size);
                    }

                    wd_tcp_message_release(&input_message);
                }
                else if (input_status != WD_TCP_READER_NEED_MORE)
                {
                    if (input_status == WD_TCP_READER_TIMED_OUT)
                    {
                        WD_LOG_WARN("input TCP channel frame timed out");
                    }
                    wd_tcp_reader_reset(&input_reader);
                    close(input_tcp_fd);
                    input_tcp_fd = -1;

                    pthread_mutex_lock(&net->lock);
                    if (net->input_tcp_fd >= 0)
                    {
                        net->input_tcp_fd = -1;
                        net->stats.tcp_input_channel_closed++;
                    }
                    pthread_mutex_unlock(&net->lock);
                    break;
                }
            }

            if (have_select_pfd && ((pfds[select_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) ||
                                         wd_tcp_reader_has_partial_frame(&selection_reader)))
            {
                struct wd_tcp_message selection_message;
                enum wd_tcp_reader_status selection_status = wd_tcp_reader_receive(
                    &selection_reader, selection_tcp_fd, receive_now_ns, WD_TCP_FRAME_TIMEOUT_NS, &selection_message);

                if (selection_status == WD_TCP_READER_MESSAGE &&
                    !wd_protocol_message_allowed(selection_message.message_type, WD_PROTOCOL_CHANNEL_SELECTION,
                                                 WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
                                                 selection_message.payload_size))
                {
                    WD_LOG_WARN("rejected selection channel message=%s(%u) size=%u",
                                wd_protocol_message_name(selection_message.message_type), selection_message.message_type,
                                selection_message.payload_size);
                    wd_tcp_message_release(&selection_message);
                    selection_status = WD_TCP_READER_INVALID_FRAME;
                }

                if (selection_status == WD_TCP_READER_MESSAGE)
                {
                    if ((selection_message.message_type == WD_MSG_CLIPBOARD_SET || selection_message.message_type == WD_MSG_PRIMARY_SET) &&
                        selection_message.payload_size >= sizeof(struct wd_selection_payload_header))
                    {
                        pthread_mutex_lock(&net->lock);
                        net->stats.tcp_selection_channel_rx++;
                        wd_clipboard_queue_client_set_locked(net, cfg.session_id, cfg.connection_token, selection_message.payload,
                                                             selection_message.payload_size,
                                                             selection_message.message_type == WD_MSG_PRIMARY_SET);
                        pthread_mutex_unlock(&net->lock);
                        wd_server_wake_input(server);
                    }
                    else if ((selection_message.message_type == WD_MSG_CLIPBOARD_REQUEST ||
                              selection_message.message_type == WD_MSG_PRIMARY_REQUEST) &&
                             selection_message.payload_size == 0)
                    {
                        pthread_mutex_lock(&net->lock);
                        net->stats.tcp_selection_channel_rx++;
                        wd_clipboard_queue_client_request_locked(net, selection_message.message_type == WD_MSG_PRIMARY_REQUEST);
                        pthread_mutex_unlock(&net->lock);
                        wd_server_wake_input(server);
                    }

                    wd_tcp_message_release(&selection_message);
                }
                else if (selection_status != WD_TCP_READER_NEED_MORE)
                {
                    if (selection_status == WD_TCP_READER_TIMED_OUT)
                    {
                        WD_LOG_WARN("selection TCP channel frame timed out");
                    }
                    wd_tcp_reader_reset(&selection_reader);
                    close(selection_tcp_fd);
                    selection_tcp_fd = -1;

                    pthread_mutex_lock(&net->lock);
                    if (net->selection_tcp_fd >= 0)
                    {
                        net->selection_tcp_fd = -1;
                        net->stats.tcp_selection_channel_closed++;
                    }
                    pthread_mutex_unlock(&net->lock);
                    break;
                }
            }

            if (have_video_pfd && ((pfds[video_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) ||
                                        wd_tcp_reader_has_partial_frame(&video_reader)))
            {
                struct wd_tcp_message video_message;
                enum wd_tcp_reader_status video_status =
                    wd_tcp_reader_receive(&video_reader, video_tcp_fd, receive_now_ns, WD_TCP_FRAME_TIMEOUT_NS, &video_message);

                if (video_status == WD_TCP_READER_MESSAGE &&
                    !wd_protocol_message_allowed(video_message.message_type, WD_PROTOCOL_CHANNEL_VIDEO,
                                                 WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
                                                 video_message.payload_size))
                {
                    WD_LOG_WARN("rejected video channel message=%s(%u) size=%u", wd_protocol_message_name(video_message.message_type),
                                video_message.message_type, video_message.payload_size);
                    wd_tcp_message_release(&video_message);
                    video_status = WD_TCP_READER_INVALID_FRAME;
                }

                if (video_status == WD_TCP_READER_MESSAGE)
                {
                    pthread_mutex_lock(&net->lock);
                    net->stats.tcp_video_channel_rx++;
                    pthread_mutex_unlock(&net->lock);
                    wd_tcp_message_release(&video_message);
                }
                else if (video_status != WD_TCP_READER_NEED_MORE)
                {
                    if (video_status == WD_TCP_READER_TIMED_OUT)
                    {
                        WD_LOG_WARN("video TCP channel frame timed out");
                    }
                    wd_tcp_reader_reset(&video_reader);
                    close(video_tcp_fd);
                    video_tcp_fd = -1;

                    pthread_mutex_lock(&net->lock);
                    if (net->video_tcp_fd >= 0)
                    {
                        net->video_tcp_fd = -1;
                        net->stats.tcp_video_channel_closed++;
                        wd_stream_video_reset_locked(server, "video channel closed", false, false);
                    }
                    pthread_mutex_unlock(&net->lock);
                }
            }

            if (have_audio_pfd && (pfds[audio_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            {
                wd_audio_stream_stop(net->audio_stream);
                close(audio_tcp_fd);
                audio_tcp_fd = -1;
                pthread_mutex_lock(&net->lock);
                net->audio_tcp_fd = -1;
                pthread_mutex_unlock(&net->lock);
                WD_LOG_INFO("audio TCP channel closed");
            }

            if ((pfds[control_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) ||
                wd_tcp_reader_has_partial_frame(&control_reader))
            {
                struct wd_tcp_message control_message;
                const enum wd_tcp_reader_status control_status =
                    wd_tcp_reader_receive(&control_reader, tcp_fd, receive_now_ns, WD_TCP_FRAME_TIMEOUT_NS, &control_message);
                if (control_status == WD_TCP_READER_NEED_MORE)
                {
                    continue;
                }
                if (control_status != WD_TCP_READER_MESSAGE)
                {
                    if (control_status == WD_TCP_READER_TIMED_OUT)
                    {
                        WD_LOG_WARN("control TCP channel frame timed out");
                    }
                    break;
                }

                type         = control_message.message_type;
                payload      = control_message.payload;
                payload_size = control_message.payload_size;

                if (!wd_protocol_message_allowed(type, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                 WD_PROTOCOL_CLIENT_TO_SERVER, payload_size))
                {
                    WD_LOG_WARN("rejected control message=%s(%u) size=%u", wd_protocol_message_name(type), type, payload_size);
                    free(payload);
                    break;
                }

                if (type == WD_MSG_TILE_REPAIR_REQUEST && payload_size >= sizeof(struct wd_tile_repair_request_payload_header))
                {
                    struct wd_tile_repair_request_payload_header rh;
                    memcpy(&rh, payload, sizeof(rh));

                    const bool exact_size =
                        wd_counted_payload_size_is_valid(payload_size, sizeof(rh), rh.request_count, sizeof(struct wd_tile_repair_entry));

                    if (rh.session_id == cfg.session_id && rh.connection_token == cfg.connection_token && exact_size)
                    {
                        struct wd_tile_repair_entry* entries = (struct wd_tile_repair_entry*)(payload + sizeof(rh));

                        pthread_mutex_lock(&net->lock);
                        if (rh.content_epoch != net->content_epoch || !wd_tile_repair_count_is_valid(rh.request_count, server->total_tiles))
                        {
                            pthread_mutex_unlock(&net->lock);
                            free(payload);
                            continue;
                        }

                        net->stats.retx_req_rx++;

                        uint64_t accepted_retransmits = 0;

                        for (uint16_t i = 0; i < rh.request_count; ++i)
                        {
                            if (entries[i].tile_id >= server->total_tiles)
                            {
                                continue;
                            }

                            struct wd_tile_state* tile                 = &net->tiles[entries[i].tile_id];
                            uint64_t              requested_generation = entries[i].requested_generation;

                            if (requested_generation != 0 && tile->generation < requested_generation)
                            {
                                /* Queue the request instead of dropping it; the
                                 * stream path will hold it until the requested
                                 * generation exists or fresh damage supersedes it. */
                            }

                            if (requested_generation != 0 && tile->generation > requested_generation)
                            {
                                /* The client asked for an older generation. It
                                 * still needs this tile, so repair it with the
                                 * latest framebuffer instead of dropping the
                                 * request and waiting for another summary. */
                                net->stats.retx_req_upgraded_generation++;
                                requested_generation = 0;
                            }

                            if (wd_stream_queue_retransmit_tile_locked(server, entries[i].tile_id, requested_generation))
                            {
                                accepted_retransmits++;
                            }
                        }

                        net->stats.retx_tiles_req += accepted_retransmits;

                        pthread_mutex_unlock(&net->lock);
                    }
                }
                else if (type == WD_MSG_CONFIG_APPLIED && payload_size == sizeof(struct wd_config_applied_payload))
                {
                    struct wd_config_applied_payload applied;
                    memcpy(&applied, payload, sizeof(applied));

                    pthread_mutex_lock(&net->lock);
                    if (net->config_update_pending &&
                        wd_config_applied_matches(&applied, net->session_id, net->connection_token, net->config_epoch))
                    {
                        const uint64_t now_ns      = wd_now_ns();
                        net->config_update_pending = false;
                        net->stats.tcp_config_applied_ack_rx++;
                        if (net->config_update_sent_ns != 0 && now_ns >= net->config_update_sent_ns)
                        {
                            const uint64_t wait_ns = now_ns - net->config_update_sent_ns;
                            net->stats.tcp_config_apply_ack_samples++;
                            net->stats.tcp_config_apply_ack_sum_ns += wait_ns;
                            if (wait_ns > net->stats.tcp_config_apply_ack_max_ns)
                            {
                                net->stats.tcp_config_apply_ack_max_ns = wait_ns;
                            }
                        }
                        net->config_update_sent_ns = 0;

                        /* Start the post-resize refresh from current scene
                         * contents only after the client has installed the
                         * matching geometry/session. Reset pacing so the
                         * first refresh is not delayed by the old FPS slot. */
                        net->stream_policy.last_frame_send_ns             = 0;
                        net->stream_policy.client_render_pressure_seconds = 0;
                        net->stream_policy.frame_rate_good_seconds        = 0;
                        wd_stream_invalidate_all_tiles_locked(server);
                        wd_server_request_full_refresh(server);
                    }
                    pthread_mutex_unlock(&net->lock);
                }
                else if (type == WD_MSG_CLIENT_STATS && payload_size == sizeof(struct wd_client_stats_payload))
                {
                    struct wd_client_stats_payload cs;
                    memcpy(&cs, payload, sizeof(cs));

                    if (cs.session_id == cfg.session_id && cs.connection_token == cfg.connection_token &&
                        (cs.flags & ~WD_CLIENT_STATS_FLAG_MASK) == 0)
                    {
                        pthread_mutex_lock(&net->lock);
                        net->stats.client_stats_rx++;
                        net->stats.client_udp_packets_rx += cs.udp_packets_rx;
                        net->stats.client_udp_bytes_rx += cs.udp_bytes_rx;
                        net->stats.client_tiles_completed += cs.udp_tiles_completed;
                        net->stats.client_completed_packets += cs.udp_completed_packets;
                        net->stats.client_partial_tiles_timed_out += cs.partial_tiles_timed_out;
                        net->stats.client_old_generation_tiles += cs.udp_ignored_old_generation;
                        net->stats.client_retx_requests_tx += cs.retx_requests_tx;
                        net->stats.client_udp_interarrival_samples += cs.udp_interarrival_samples;
                        net->stats.client_udp_interarrival_sum_ns += cs.udp_interarrival_sum_ns;
                        net->stats.client_udp_interarrival_jitter_samples += cs.udp_interarrival_jitter_samples;
                        net->stats.client_udp_interarrival_jitter_sum_ns += cs.udp_interarrival_jitter_sum_ns;
                        if ((cs.flags & WD_CLIENT_STATS_RENDER_VISIBLE) != 0)
                        {
                            net->stats.client_render_visible_reports++;
                        }
                        else
                        {
                            net->stats.client_render_hidden_reports++;
                        }
                        if (cs.udp_interarrival_max_ns > net->stats.client_udp_interarrival_max_ns)
                        {
                            net->stats.client_udp_interarrival_max_ns = cs.udp_interarrival_max_ns;
                        }
                        net->stats.client_render_frames += cs.render_frames;
                        net->stats.client_present_samples += cs.present_samples;
                        net->stats.client_present_sum_ns += cs.present_sum_ns;
                        if (cs.present_max_ns > net->stats.client_present_max_ns)
                        {
                            net->stats.client_present_max_ns = cs.present_max_ns;
                        }
                        net->stats.client_input_present_samples += cs.input_present_samples;
                        net->stats.client_input_present_sum_ns += cs.input_present_sum_ns;
                        net->stats.client_video_frames_rx += cs.video_frames_rx;
                        net->stats.client_video_bytes_rx += cs.video_bytes_rx;
                        net->stats.client_video_frames_decoded += cs.video_frames_decoded;
                        net->stats.client_video_frames_presented += cs.video_frames_presented;
                        net->stats.client_video_decode_failed += cs.video_decode_failed;
                        net->stats.client_video_publish_failed += cs.video_publish_failed;
                        net->stats.client_video_control_frames_rx += cs.video_control_frames_rx;
                        net->stats.client_video_need_keyframe_drops += cs.video_need_keyframe_drops;
                        net->stats.client_video_decoder_resets += cs.video_decoder_resets;
                        net->stats.client_video_decode_samples += cs.video_decode_samples;
                        net->stats.client_video_decode_sum_ns += cs.video_decode_sum_ns;
                        net->stats.client_video_messages_rx += cs.video_messages_rx;
                        net->stats.client_video_data_frames_rx += cs.video_data_frames_rx;
                        net->stats.client_video_invalid_frames_rx += cs.video_invalid_frames_rx;
                        net->stats.client_video_stale_frames_dropped += cs.video_stale_frames_dropped;
                        if (cs.video_last_frame_id_rx > net->stats.client_video_last_frame_id_rx)
                        {
                            net->stats.client_video_last_frame_id_rx = cs.video_last_frame_id_rx;
                        }
                        if (cs.video_last_frame_id_presented > net->stats.client_video_last_frame_id_presented)
                        {
                            net->stats.client_video_last_frame_id_presented = cs.video_last_frame_id_presented;
                        }
                        net->stats.client_video_present_latency_samples += cs.video_present_latency_samples;
                        net->stats.client_video_present_latency_sum_ns += cs.video_present_latency_sum_ns;
                        net->stats.client_audio_messages_rx += cs.audio_messages_rx;
                        net->stats.client_audio_packets_rx += cs.audio_packets_rx;
                        net->stats.client_audio_bytes_rx += cs.audio_bytes_rx;
                        net->stats.client_audio_decode_failed += cs.audio_decode_failed;
                        net->stats.client_audio_discontinuities += cs.audio_discontinuities;
                        net->stats.client_audio_late_drops += cs.audio_late_drops;
                        net->stats.client_audio_underflows += cs.audio_underflows;
                        net->stats.client_audio_video_sync_holds += cs.audio_video_sync_holds;
                        net->stats.client_audio_video_sync_drops += cs.audio_video_sync_drops;
                        net->stats.client_video_queue_overflow_drops += cs.video_queue_overflow_drops;
                        net->stats.client_video_queue_depth = cs.video_queue_depth;
                        if (cs.video_queue_depth_max > net->stats.client_video_queue_depth_max)
                        {
                            net->stats.client_video_queue_depth_max = cs.video_queue_depth_max;
                        }
                        net->stats.client_video_oldest_pts_usec     = cs.video_oldest_pts_usec;
                        net->stats.client_audio_video_delta_samples = cs.audio_video_delta_samples;
                        net->stats.client_tile_frames_presented += cs.tile_frames_presented;
                        pthread_mutex_unlock(&net->lock);
                    }
                }
                else if (type == WD_MSG_DISPLAY_RESIZE && payload_size == sizeof(struct wd_display_resize_payload))
                {
                    struct wd_display_resize_payload resize;
                    memcpy(&resize, payload, sizeof(resize));

                    if (resize.session_id == cfg.session_id && resize.connection_token == cfg.connection_token && resize.width != 0 &&
                        resize.height != 0)
                    {
                        if (wd_server_request_display_size(server, resize.width, resize.height))
                        {
                            pthread_mutex_lock(&net->lock);
                            bool config_sent = wd_server_send_current_config_locked(server);
                            if (config_sent)
                            {
                                wd_server_fill_config(server, net->session_id, selected_udp_payload, &cfg);
                            }
                            pthread_mutex_unlock(&net->lock);
                            if (!config_sent)
                            {
                                free(payload);
                                break;
                            }

                            WD_LOG_INFO("client resized display to %ux%u", server->display_width, server->display_height);
                        }
                        else
                        {
                            WD_LOG_ERROR("rejected display resize to %ux%u", resize.width, resize.height);
                        }
                    }
                }

                free(payload);
            }
        }

        wd_tcp_reader_destroy(&control_reader);
        wd_tcp_reader_destroy(&input_reader);
        wd_tcp_reader_destroy(&selection_reader);
        wd_tcp_reader_destroy(&video_reader);

        pthread_mutex_lock(&net->lock);

        if (net->tcp_fd == tcp_fd)
        {
            net->tcp_fd = -1;
        }
        if (net->input_tcp_fd == input_tcp_fd)
        {
            net->input_tcp_fd = -1;
        }
        if (net->selection_tcp_fd == selection_tcp_fd)
        {
            net->selection_tcp_fd = -1;
        }
        if (net->video_tcp_fd == video_tcp_fd)
        {
            net->video_tcp_fd = -1;
        }
        if (net->audio_tcp_fd == audio_tcp_fd)
        {
            net->audio_tcp_fd = -1;
        }

        net->client_connected = false;
        net->connection_epoch++;
        if (net->connection_epoch == 0)
        {
            net->connection_epoch = 1;
        }
        net->content_epoch++;
        if (net->content_epoch == 0)
        {
            net->content_epoch = 1;
        }
        net->config_update_pending = false;
        net->config_update_sent_ns = 0;
        wd_stream_video_reset_locked(server, "client disconnected", false, false);
        net->video_stream_negotiated = false;
        net->video_codecs            = 0;
        net->video_transport         = 0;
        net->audio_stream_negotiated = false;
        net->audio_codec             = 0;
        net->audio_transport         = 0;
        net->audio_channels          = 0;
        net->audio_target_latency_ms = 0;
        net->audio_bitrate           = 0;
        net->key_queue_count         = 0;
        net->pointer_queue_count     = 0;
        net->key_state_reset_pending = true;

        free(net->clipboard_text);
        net->clipboard_text         = NULL;
        net->clipboard_text_size    = 0;
        net->clipboard_text_pending = false;

        free(net->primary_text);
        net->primary_text              = NULL;
        net->primary_text_size         = 0;
        net->primary_text_pending      = false;
        net->clipboard_request_pending = false;
        net->primary_request_pending   = false;

        pthread_mutex_unlock(&net->lock);
        wd_server_wake_input(server);

        wd_audio_stream_stop(net->audio_stream);
        close(tcp_fd);
        if (input_tcp_fd >= 0)
        {
            close(input_tcp_fd);
            input_tcp_fd = -1;
        }
        if (selection_tcp_fd >= 0)
        {
            close(selection_tcp_fd);
            selection_tcp_fd = -1;
        }
        if (video_tcp_fd >= 0)
        {
            close(video_tcp_fd);
            video_tcp_fd = -1;
        }
        if (audio_tcp_fd >= 0)
        {
            close(audio_tcp_fd);
            audio_tcp_fd = -1;
        }

        WD_LOG_INFO("client disconnected; waiting for reconnect");
    }

    wd_close_fd(&net->tcp_fd);
    wd_close_fd(&net->input_tcp_fd);
    wd_close_fd(&net->selection_tcp_fd);
    wd_audio_stream_stop(net->audio_stream);
    wd_close_fd(&net->video_tcp_fd);
    wd_close_fd(&net->audio_tcp_fd);
    wd_close_fd(&net->udp_fd);
    wd_close_fd(&net->listen_fd);

    return NULL;
}
