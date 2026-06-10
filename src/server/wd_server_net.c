#include "waydisplay/wd_net.h"
#include "waydisplay/wd_time.h"
#include "wd_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>


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
    WD_LOG_INFO("WayDisplay: %s TCP channel connected local=%s remote=%s", channel, local, remote);
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

bool wd_net_init(struct wd_server* server, uint16_t tcp_port) {
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

    if (pthread_cond_init(&net->encoder_idle_cond, NULL) != 0)
    {
        pthread_cond_destroy(&net->display_resize_cond);
        pthread_mutex_destroy(&net->lock);
        return false;
    }

    net->running              = true;
    net->tcp_port             = tcp_port;
    net->tcp_fd               = -1;
    net->input_tcp_fd         = -1;
    net->selection_tcp_fd     = -1;
    net->listen_fd            = -1;
    net->udp_fd               = -1;
    net->session_id           = 0;
    net->dirty_region_rng = 0;
    net->dirty_regions               = calloc(server->total_tiles, sizeof(*net->dirty_regions));
    net->dirty_region_queued         = calloc(server->total_tiles, sizeof(*net->dirty_region_queued));
    net->dirty_region_count          = 0;
    net->dirty_epochs                = calloc(server->total_tiles, sizeof(*net->dirty_epochs));
    net->dirty_queue                 = calloc(server->total_tiles, sizeof(*net->dirty_queue));
    net->dirty_queued                = calloc(server->total_tiles, sizeof(*net->dirty_queued));
    net->dirty_queue_enqueued_ns     = calloc(server->total_tiles, sizeof(*net->dirty_queue_enqueued_ns));
    net->retransmit_queue            = calloc(server->total_tiles, sizeof(*net->retransmit_queue));
    net->retransmit_queued           = calloc(server->total_tiles, sizeof(*net->retransmit_queued));
    net->retransmit_queue_enqueued_ns = calloc(server->total_tiles, sizeof(*net->retransmit_queue_enqueued_ns));
    net->retransmit_requested_generation = calloc(server->total_tiles, sizeof(*net->retransmit_requested_generation));
    net->summary_dirty_tiles         = calloc(server->total_tiles, sizeof(*net->summary_dirty_tiles));
    if (!net->dirty_regions || !net->dirty_region_queued || !net->dirty_epochs || !net->dirty_queue || !net->dirty_queued ||
        !net->dirty_queue_enqueued_ns || !net->retransmit_queue ||
        !net->retransmit_queued || !net->retransmit_queue_enqueued_ns || !net->retransmit_requested_generation ||
        !net->summary_dirty_tiles)
    {
        free(net->dirty_regions);
        free(net->dirty_region_queued);
        free(net->dirty_epochs);
        free(net->dirty_queue);
        free(net->dirty_queued);
        free(net->dirty_queue_enqueued_ns);
        free(net->retransmit_queue);
        free(net->retransmit_queued);
        free(net->retransmit_queue_enqueued_ns);
        free(net->retransmit_requested_generation);
        free(net->summary_dirty_tiles);
        net->dirty_regions               = NULL;
        net->dirty_region_queued         = NULL;
        net->dirty_region_count          = 0;
        net->dirty_epochs                = NULL;
        net->dirty_queue                 = NULL;
        net->dirty_queued                = NULL;
        net->dirty_queue_enqueued_ns     = NULL;
        net->retransmit_queue            = NULL;
        net->retransmit_queued           = NULL;
        net->retransmit_queue_enqueued_ns = NULL;
        net->retransmit_requested_generation = NULL;
        net->summary_dirty_tiles         = NULL;
        pthread_cond_destroy(&net->encoder_idle_cond);
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
    net->udp_payload_target            = WD_UDP_PAYLOAD_TARGET;

    wd_stream_policy_set_defaults(&net->stream_policy);

    return true;
}

void wd_net_destroy(struct wd_server* server) {
    struct wd_net_state* net = &server->net;

    net->running = false;

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

    if (net->udp_fd >= 0)
    {
        close(net->udp_fd);
        net->udp_fd = -1;
    }

    free(net->dirty_regions);
    net->dirty_regions = NULL;
    free(net->dirty_region_queued);
    net->dirty_region_queued = NULL;
    free(net->dirty_epochs);
    net->dirty_epochs = NULL;
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
    net->summary_dirty_count = 0;

    free(net->clipboard_text);
    net->clipboard_text         = NULL;
    net->clipboard_text_size    = 0;
    net->clipboard_text_pending = false;

    free(net->primary_text);
    net->primary_text         = NULL;
    net->primary_text_size    = 0;
    net->primary_text_pending = false;

    wd_stream_destroy(server);

    pthread_cond_destroy(&net->encoder_idle_cond);
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
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }

#if defined(IP_MTU_DISCOVER)
#    if defined(IP_PMTUDISC_PROBE)
    const int mode = IP_PMTUDISC_PROBE;
#    elif defined(IP_PMTUDISC_DO)
    const int mode = IP_PMTUDISC_DO;
#    else
    const int mode = -1;
#    endif

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
    struct wd_net_state* net = &server->net;

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
        WD_IPV4_MTU_TO_TILE_PAYLOAD(9000u),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(8192u),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(4096u),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(1500u),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(1492u),
        WD_IPV4_MTU_TO_TILE_PAYLOAD(1460u),
        1400,
        1360,
        1300,
        1200,
    };

    const uint16_t probe_count = (uint16_t)(sizeof(probe_sizes) / sizeof(probe_sizes[0]));

    struct wd_mtu_probe_start_payload start;
    memset(&start, 0, sizeof(start));
    start.session_id  = net->session_id;
    start.probe_count = probe_count;

    if (!wd_send_tcp_message(tcp_fd, WD_MSG_MTU_PROBE_START, &start, sizeof(start)))
    {
        return WD_UDP_PAYLOAD_TARGET;
    }

    int probe_udp_fd = wd_create_udp_mtu_probe_socket();
    if (probe_udp_fd < 0)
    {
        WD_LOG_INFO("WayDisplay: UDP MTU probe could not force DF; using default UDP payload target: %u", WD_UDP_PAYLOAD_TARGET);
        wd_udp_socket_disable_df_best_effort(net->udp_fd);
        return WD_UDP_PAYLOAD_TARGET;
    }

    /*
     * Give the client a moment to enter its UDP-probe receive loop.
     * Probe packets are sent with IPv4 DF set so successful receipt means
     * the datagram fit the path MTU instead of being IP-fragmented and
     * reassembled.
     */
    wd_sleep_ms(10);

    uint8_t packet[WD_UDP_TILE_HEADER_MAX_SIZE + WD_UDP_TILE_PAYLOAD_MAX];

    for (uint16_t i = 0; i < probe_count; ++i)
    {
        uint16_t payload_size = probe_sizes[i];
        size_t   packet_size  = sizeof(struct wd_udp_tile_packet_header_compressed_multi) + payload_size;

        memset(packet, 0, sizeof(packet));

        struct wd_udp_tile_packet_header_compressed_multi* h = (struct wd_udp_tile_packet_header_compressed_multi*)packet;

        h->session_id           = net->session_id;
        h->tile_protocol        = WD_TILE_COMPRESSED_MULTI;
        h->tile_flags           = WD_TILE_NORMAL;
        h->tile_size            = wd_tile_size_code_for_dimensions(server->tile_width, server->tile_height);
        h->tile_id              = WD_UDP_TILE_ID_MTU_PROBE;
        h->tile_pkt_count       = probe_count > UINT8_MAX ? UINT8_MAX : (uint8_t)probe_count;
        h->tile_pkt_id          = i > UINT8_MAX ? UINT8_MAX : (uint8_t)i;
        h->payload_size         = payload_size;
        h->tile_generation      = net->session_id;
        h->compressed_tile_size = payload_size;

        memset(packet + sizeof(*h), 0xa5, payload_size);

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

    if (type == WD_MSG_MTU_PROBE_RESULT && payload_size >= sizeof(struct wd_mtu_probe_result_payload))
    {
        struct wd_mtu_probe_result_payload probe_result;
        memcpy(&probe_result, payload, sizeof(probe_result));

        if (probe_result.session_id == net->session_id && probe_result.max_udp_payload_received >= WD_MIN_PROBED_UDP_PAYLOAD)
        {
            result = probe_result.max_udp_payload_received;
        }
    }

    free(payload);

    if (result > WD_UDP_TILE_PAYLOAD_MAX)
    {
        result = WD_UDP_TILE_PAYLOAD_MAX;
    }

    if (result < 512)
    {
        result = WD_UDP_PAYLOAD_TARGET;
    }

    WD_LOG_INFO("WayDisplay: UDP payload target selected by probe: %u", result);

    return result;
}

static uint64_t run_udp_throughput_probe(struct wd_server* server, int tcp_fd, const struct sockaddr_in* client_udp_addr,
                                         uint16_t udp_payload_target) {
    struct wd_net_state* net = &server->net;

    if (udp_payload_target == 0 || udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    if (udp_payload_target < WD_MIN_PROBED_UDP_PAYLOAD)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    size_t packet_size = sizeof(struct wd_udp_tile_packet_header_compressed_multi) + udp_payload_target;

    struct wd_throughput_probe_start_payload start;
    memset(&start, 0, sizeof(start));
    start.session_id  = net->session_id;
    /*
     * UINT8_MAX means the probe is duration-limited. Earlier code sent a
     * fixed WD_THROUGHPUT_PROBE_TARGET_BYTES budget paced across the probe
     * window, which capped the best possible localhost result to roughly
     * 8 MiB / 750 ms * safety ~= 9 MiB/s. Keep the UDP tile probe header in
     * its normal uint8_t packet-count shape, but repeat packet ids and let the
     * client count every received probe datagram until the deadline.
     */
    start.probe_count = UINT8_MAX;
    start.payload_size = udp_payload_target;
    start.duration_ms = WD_THROUGHPUT_PROBE_DURATION_MS;

    if (!wd_send_tcp_message(tcp_fd, WD_MSG_THROUGHPUT_PROBE_START, &start, sizeof(start)))
    {
        return WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }

    wd_udp_socket_disable_df_best_effort(net->udp_fd);

    /* Give the client a moment to switch from MTU probing to throughput probing. */
    wd_sleep_ms(10);

    uint8_t* packet = malloc(packet_size);
    if (!packet)
    {
        return WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }

    memset(packet, 0, packet_size);
    struct wd_udp_tile_packet_header_compressed_multi* h = (struct wd_udp_tile_packet_header_compressed_multi*)packet;
    h->session_id           = net->session_id;
    h->tile_protocol        = WD_TILE_COMPRESSED_MULTI;
    h->tile_flags           = WD_TILE_NORMAL;
    h->tile_size            = wd_tile_size_code_for_dimensions(server->tile_width, server->tile_height);
    h->tile_id              = WD_UDP_TILE_ID_THROUGHPUT_PROBE;
    h->tile_pkt_count       = (uint8_t)start.probe_count;
    h->payload_size         = udp_payload_target;
    h->tile_generation      = net->session_id;
    h->compressed_tile_size = udp_payload_target;
    memset(packet + sizeof(*h), 0x5a, udp_payload_target);

    const uint64_t start_ns    = wd_now_ns();
    const uint64_t duration_ns = (uint64_t)WD_THROUGHPUT_PROBE_DURATION_MS * 1000000ull;
    const uint64_t deadline_ns = start_ns + duration_ns;

    uint32_t packets_sent = 0;
    while (wd_now_ns() < deadline_ns)
    {
        h->tile_pkt_id = (uint8_t)(packets_sent % UINT8_MAX);

        ssize_t sent = sendto(net->udp_fd, packet, packet_size, 0, (const struct sockaddr*)client_udp_addr, sizeof(*client_udp_addr));
        if (sent == (ssize_t)packet_size)
        {
            packets_sent++;
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS))
        {
            wd_sleep_ms(1);
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

    uint16_t type         = 0;
    uint8_t* payload      = NULL;
    uint32_t payload_size_rx = 0;

    if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size_rx))
    {
        free(payload);
        return WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    }

    uint64_t limited_rate = WD_LIMITED_MODE_DEFAULT_UDP_BYTES_PER_SECOND;
    uint32_t packets_received = 0;

    if (type == WD_MSG_THROUGHPUT_PROBE_RESULT && payload_size_rx >= sizeof(struct wd_throughput_probe_result_payload))
    {
        struct wd_throughput_probe_result_payload result;
        memcpy(&result, payload, sizeof(result));
        packets_received = result.packets_received;

        if (result.session_id == net->session_id && result.bytes_received > 0 && result.duration_ms > 0)
        {
            uint64_t bytes_per_second = ((uint64_t)result.bytes_received * 1000ull) / result.duration_ms;
            bytes_per_second = (bytes_per_second * WD_LIMITED_MODE_THROUGHPUT_SAFETY_PERCENT) / 100ull;

            if (bytes_per_second < WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND)
            {
                bytes_per_second = WD_LIMITED_MODE_MIN_UDP_BYTES_PER_SECOND;
            }
            if (bytes_per_second > WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND)
            {
                bytes_per_second = WD_LIMITED_MODE_MAX_UDP_BYTES_PER_SECOND;
            }

            limited_rate = bytes_per_second;
        }
    }

    free(payload);

    WD_LOG_INFO("WayDisplay: adaptive UDP byte budget selected by throughput probe: %llu KiB/s sent=%u recv=%u",
                (unsigned long long)(limited_rate / 1024ull), packets_sent, packets_received);

    return limited_rate;
}

static void wd_server_fill_config(struct wd_server* server, uint8_t session_id, uint16_t udp_payload_target,
                                  struct wd_server_config_payload* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->session_id         = session_id;
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
    cfg->capabilities       = WD_SERVER_CAP_INPUT_CHANNEL | WD_SERVER_CAP_SELECTION_CHANNEL;
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

    if (!wd_send_tcp_message(net->tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg)))
    {
        return false;
    }

    net->stats.tcp_config_tx++;
    return true;
}

static void wd_server_handle_keyboard_message(struct wd_server* server, const struct wd_server_config_payload* cfg,
                                             const uint8_t* payload, uint32_t payload_size) {
    struct wd_net_state* net = &server->net;

    if (payload_size < sizeof(struct wd_keyboard_event_payload))
    {
        return;
    }

    struct wd_keyboard_event_payload key;
    memcpy(&key, payload, sizeof(key));

    if (key.session_id == cfg->session_id && key.evdev_key_code != 0)
    {
        pthread_mutex_lock(&net->lock);
        wd_keyboard_queue_event_locked(net, &key, wd_now_ns());
        pthread_mutex_unlock(&net->lock);
    }
}

static void wd_server_handle_pointer_message(struct wd_server* server, const struct wd_server_config_payload* cfg,
                                            const uint8_t* payload, uint32_t payload_size) {
    struct wd_net_state* net = &server->net;

    if (payload_size < sizeof(struct wd_pointer_event_payload))
    {
        return;
    }

    struct wd_pointer_event_payload pointer;
    memcpy(&pointer, payload, sizeof(pointer));

    if (pointer.session_id == cfg->session_id)
    {
        if (pointer.event_type == WD_POINTER_EVENT_BUTTON && pointer.button == 0x111)
        {
            WD_LOG_DEBUG("WayDisplay: received right click %s from client "
                         "x=%u y=%u mods=0x%x timestamp=%" PRIu64,
                         pointer.button_state == WD_POINTER_BUTTON_PRESSED ? "press" : "release", pointer.x, pointer.y,
                         pointer.modifiers, pointer.client_timestamp_ns);
        }
        pthread_mutex_lock(&net->lock);
        wd_pointer_queue_event_locked(net, &pointer, wd_now_ns());
        pthread_mutex_unlock(&net->lock);
    }
}

static bool wd_accept_aux_channel_fd(struct wd_server* server, uint8_t session_id, int* input_tcp_fd, int* selection_tcp_fd) {
    struct wd_net_state* net = &server->net;

    struct sockaddr_in peer_addr;
    socklen_t          peer_len = sizeof(peer_addr);
    int                fd       = accept(net->listen_fd, (struct sockaddr*)&peer_addr, &peer_len);

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

    if (type == WD_MSG_INPUT_CHANNEL_HELLO && payload_size >= sizeof(struct wd_input_channel_hello_payload) && *input_tcp_fd < 0)
    {
        struct wd_input_channel_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));

        if (hello.session_id == session_id)
        {
            *input_tcp_fd = fd;
            accepted      = true;
        }
    }
    else if (type == WD_MSG_SELECTION_CHANNEL_HELLO && payload_size >= sizeof(struct wd_selection_channel_hello_payload) &&
             *selection_tcp_fd < 0)
    {
        struct wd_selection_channel_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));

        if (hello.session_id == session_id)
        {
            *selection_tcp_fd = fd;
            accepted          = true;
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

static void wd_accept_optional_aux_channels(struct wd_server* server, uint8_t session_id, int* input_tcp_fd, int* selection_tcp_fd) {
    struct wd_net_state* net = &server->net;
    const uint64_t       deadline_ns = wd_now_ns() + 500ull * 1000ull * 1000ull;

    *input_tcp_fd     = -1;
    *selection_tcp_fd = -1;

    while ((*input_tcp_fd < 0 || *selection_tcp_fd < 0) && wd_now_ns() < deadline_ns)
    {
        uint64_t now_ns = wd_now_ns();
        if (now_ns >= deadline_ns)
        {
            break;
        }

        uint64_t remaining_ns = deadline_ns - now_ns;
        int      timeout_ms   = (int)(remaining_ns / (1000ull * 1000ull));
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

        (void)wd_accept_aux_channel_fd(server, session_id, input_tcp_fd, selection_tcp_fd);
    }
}

void* wd_net_thread_main(void* arg) {
    struct wd_server*    server = arg;
    struct wd_net_state* net    = &server->net;

    net->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (net->listen_fd < 0)
    {
        WD_LOG_ERROR("WayDisplay: TCP socket failed: %s", strerror(errno));
        return NULL;
    }

    int yes = 1;
    setsockopt(net->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));

    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port        = htons(net->tcp_port);

    if (bind(net->listen_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0)
    {
        WD_LOG_ERROR("WayDisplay: bind TCP failed: %s", strerror(errno));
        wd_close_fd(&net->listen_fd);
        return NULL;
    }

    if (listen(net->listen_fd, 3) < 0)
    {
        WD_LOG_ERROR("WayDisplay: listen failed: %s", strerror(errno));
        wd_close_fd(&net->listen_fd);
        return NULL;
    }

    net->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (net->udp_fd < 0)
    {
        WD_LOG_ERROR("WayDisplay: UDP socket failed: %s", strerror(errno));
        wd_close_fd(&net->listen_fd);
        return NULL;
    }

    struct sockaddr_in udp_bind_addr;
    memset(&udp_bind_addr, 0, sizeof(udp_bind_addr));
    udp_bind_addr.sin_family      = AF_INET;
    udp_bind_addr.sin_addr.s_addr = INADDR_ANY;
    udp_bind_addr.sin_port        = htons(0);
    if (bind(net->udp_fd, (struct sockaddr*)&udp_bind_addr, sizeof(udp_bind_addr)) < 0)
    {
        WD_LOG_ERROR("WayDisplay: bind UDP sender failed: %s", strerror(errno));
    }

    if (wd_set_nonblocking(net->udp_fd) < 0)
    {
        WD_LOG_ERROR("WayDisplay: failed to make UDP socket nonblocking: %s", strerror(errno));
    }

    wd_udp_socket_disable_df_best_effort(net->udp_fd);

    {
        char tcp_local[64];
        char udp_local[64];

        wd_format_socket_endpoint(net->listen_fd, false, tcp_local, sizeof(tcp_local));
        wd_format_socket_endpoint(net->udp_fd, false, udp_local, sizeof(udp_local));
        WD_LOG_INFO("WayDisplay: network listening tcp=%s udp_sender=%s tcp_fd=%d udp_fd=%d", tcp_local, udp_local,
                    net->listen_fd, net->udp_fd);
    }

    int sndbuf = WD_UDP_SOCKET_BUFFER_BYTES;
    if (setsockopt(net->udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
    {
        WD_LOG_ERROR("WayDisplay: setsockopt SO_SNDBUF failed: %s", strerror(errno));
    }
    else
    {
        int       actual     = 0;
        socklen_t actual_len = sizeof(actual);

        if (getsockopt(net->udp_fd, SOL_SOCKET, SO_SNDBUF, &actual, &actual_len) == 0)
        {
            WD_LOG_INFO("WayDisplay: UDP send buffer requested=%d actual=%d", sndbuf, actual);
        }
    }

    while (net->running)
    {
        struct sockaddr_in peer_addr;
        socklen_t          peer_len = sizeof(peer_addr);

        int tcp_fd = accept(net->listen_fd, (struct sockaddr*)&peer_addr, &peer_len);

        if (tcp_fd < 0)
        {
            if (!net->running)
            {
                break;
            }

            if (errno == EINTR)
            {
                continue;
            }

            WD_LOG_ERROR("WayDisplay: accept failed: %s", strerror(errno));
            continue;
        }

        wd_configure_accepted_tcp_socket(tcp_fd);

        uint16_t type         = 0;
        uint8_t* payload      = NULL;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size) || type != WD_MSG_CLIENT_HELLO ||
            payload_size < sizeof(struct wd_client_hello_payload))
        {
            WD_LOG_ERROR("WayDisplay: invalid client hello");
            free(payload);
            close(tcp_fd);
            continue;
        }

        struct wd_client_hello_payload hello;
        memset(&hello, 0, sizeof(hello));
        memcpy(&hello, payload, sizeof(hello));
        free(payload);
        payload = NULL;

        if (hello.client_udp_port == 0)
        {
            WD_LOG_ERROR("WayDisplay: rejected client hello with UDP port 0");
            close(tcp_fd);
            continue;
        }

        if (hello.desired_width != 0 || hello.desired_height != 0)
        {
            if (hello.desired_width == 0 || hello.desired_height == 0 ||
                !wd_server_apply_display_size(server, hello.desired_width, hello.desired_height))
            {
                WD_LOG_ERROR("WayDisplay: rejected requested client display size %ux%u", hello.desired_width, hello.desired_height);
                close(tcp_fd);
                continue;
            }
        }

        struct wd_server_config_payload cfg;
        uint8_t                         session_id = 0;

        pthread_mutex_lock(&net->lock);

        if (net->session_id == 0)
        {
            net->session_id = (uint8_t)(wd_now_ns() ^ 0x9e3779b9u);
            if (net->session_id == 0)
            {
                net->session_id = 1;
            }
        }

        session_id = net->session_id;

        pthread_mutex_unlock(&net->lock);

        struct sockaddr_in client_udp_addr;
        memset(&client_udp_addr, 0, sizeof(client_udp_addr));

        client_udp_addr.sin_family = AF_INET;
        client_udp_addr.sin_addr   = peer_addr.sin_addr;
        client_udp_addr.sin_port   = htons(hello.client_udp_port);

        uint16_t selected_udp_payload = run_udp_mtu_probe(server, tcp_fd, &client_udp_addr);
        uint64_t selected_limited_udp_rate = run_udp_throughput_probe(server, tcp_fd, &client_udp_addr, selected_udp_payload);

        pthread_mutex_lock(&net->lock);
        net->udp_payload_target = selected_udp_payload;
        wd_stream_policy_set_limited_udp_byte_rate(&net->stream_policy, selected_limited_udp_rate);
        pthread_mutex_unlock(&net->lock);

        wd_server_fill_config(server, session_id, selected_udp_payload, &cfg);

        if (!wd_send_tcp_message(tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg)))
        {
            WD_LOG_ERROR("WayDisplay: failed to send server config");
            close(tcp_fd);
            continue;
        }

        int input_tcp_fd     = -1;
        int selection_tcp_fd = -1;
        wd_accept_optional_aux_channels(server, cfg.session_id, &input_tcp_fd, &selection_tcp_fd);

        wd_clear_tcp_receive_timeout(tcp_fd);

        pthread_mutex_lock(&net->lock);

        wd_stream_policy_apply_client_hello(&net->stream_policy, &hello);

        net->tcp_fd           = tcp_fd;
        net->input_tcp_fd     = input_tcp_fd;
        net->selection_tcp_fd = selection_tcp_fd;
        if (input_tcp_fd >= 0)
        {
            net->stats.tcp_input_channel_accepted++;
        }
        if (selection_tcp_fd >= 0)
        {
            net->stats.tcp_selection_channel_accepted++;
        }
        net->client_udp_addr      = client_udp_addr;
        net->client_connected     = true;
        net->dirty_region_rng = 0;
        if (net->dirty_region_queued)
        {
            memset(net->dirty_region_queued, 0, server->total_tiles * sizeof(*net->dirty_region_queued));
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
        server->last_summary_ns       = 0;
        server->last_delta_summary_ns = 0;

        net->stats.tcp_hello_rx++;
        net->stats.tcp_config_tx++;

        net->key_queue_count     = 0;
        net->pointer_queue_count = 0;
        net->key_state_reset_pending = true;

        free(net->clipboard_text);
        net->clipboard_text         = NULL;
        net->clipboard_text_size    = 0;
        net->clipboard_text_pending = false;

        free(net->primary_text);
        net->primary_text         = NULL;
        net->primary_text_size    = 0;
        net->primary_text_pending = false;
        wd_cursor_send_current_locked(server);

        pthread_mutex_unlock(&net->lock);

        {
            char control_local[64];
            char control_remote[64];
            char udp_local[64];
            char udp_remote[64];

            wd_format_socket_endpoint(tcp_fd, false, control_local, sizeof(control_local));
            wd_format_socket_endpoint(tcp_fd, true, control_remote, sizeof(control_remote));
            wd_format_socket_endpoint(net->udp_fd, false, udp_local, sizeof(udp_local));
            wd_format_sockaddr_in(&client_udp_addr, udp_remote, sizeof(udp_remote));

            WD_LOG_INFO("WayDisplay: client connected; control_tcp=%s<->%s udp=%s->%s input_channel=%s selection_channel=%s display=%ux%u tile=%ux%u fps=%u requested_udp_kib_per_sec=%u adaptive_udp_kib_per_sec=%llu",
                        control_local, control_remote, udp_local, udp_remote, input_tcp_fd >= 0 ? "yes" : "no",
                        selection_tcp_fd >= 0 ? "yes" : "no", server->display_width, server->display_height,
                        server->tile_width, server->tile_height, hello.target_fps, hello.limited_udp_kib_per_second,
                        (unsigned long long)(net->stream_policy.limited_udp_bytes_per_second / 1024ull));

            if (input_tcp_fd >= 0)
            {
                wd_log_tcp_channel_endpoint("input", input_tcp_fd);
            }
            if (selection_tcp_fd >= 0)
            {
                wd_log_tcp_channel_endpoint("selection", selection_tcp_fd);
            }
        }


        while (net->running)
        {
            struct pollfd pfds[4];
            nfds_t        nfds            = 0;
            nfds_t        control_pfd_idx = 0;
            nfds_t        input_pfd_idx   = 0;
            nfds_t        select_pfd_idx  = 0;
            nfds_t        listen_pfd_idx  = 0;
            bool          have_input_pfd  = false;
            bool          have_select_pfd = false;
            bool          have_listen_pfd = false;

            memset(pfds, 0, sizeof(pfds));
            control_pfd_idx       = nfds;
            pfds[nfds].fd         = tcp_fd;
            pfds[nfds].events     = POLLIN;
            nfds++;

            if (input_tcp_fd >= 0)
            {
                input_pfd_idx         = nfds;
                have_input_pfd        = true;
                pfds[nfds].fd         = input_tcp_fd;
                pfds[nfds].events     = POLLIN;
                nfds++;
            }

            if (selection_tcp_fd >= 0)
            {
                select_pfd_idx        = nfds;
                have_select_pfd       = true;
                pfds[nfds].fd         = selection_tcp_fd;
                pfds[nfds].events     = POLLIN;
                nfds++;
            }

            if (input_tcp_fd < 0 || selection_tcp_fd < 0)
            {
                listen_pfd_idx        = nfds;
                have_listen_pfd       = true;
                pfds[nfds].fd         = net->listen_fd;
                pfds[nfds].events     = POLLIN;
                nfds++;
            }

            int poll_rc;
            do
            {
                poll_rc = poll(pfds, nfds, -1);
            } while (poll_rc < 0 && errno == EINTR);

            if (poll_rc <= 0)
            {
                break;
            }


            if (have_listen_pfd && (pfds[listen_pfd_idx].revents & POLLIN))
            {
                int old_input_fd     = input_tcp_fd;
                int old_selection_fd = selection_tcp_fd;

                (void)wd_accept_aux_channel_fd(server, cfg.session_id, &input_tcp_fd, &selection_tcp_fd);

                if (input_tcp_fd >= 0 && old_input_fd < 0)
                {
                    pthread_mutex_lock(&net->lock);
                    net->input_tcp_fd = input_tcp_fd;
                    net->stats.tcp_input_channel_accepted++;
                    pthread_mutex_unlock(&net->lock);
                    wd_log_tcp_channel_endpoint("late input", input_tcp_fd);
                }

                if (selection_tcp_fd >= 0 && old_selection_fd < 0)
                {
                    pthread_mutex_lock(&net->lock);
                    net->selection_tcp_fd = selection_tcp_fd;
                    net->stats.tcp_selection_channel_accepted++;
                    pthread_mutex_unlock(&net->lock);
                    wd_log_tcp_channel_endpoint("late selection", selection_tcp_fd);
                }
            }

            if (have_input_pfd && (pfds[input_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            {
                uint16_t input_type         = 0;
                uint8_t* input_payload      = NULL;
                uint32_t input_payload_size = 0;

                if (!wd_recv_tcp_message(input_tcp_fd, &input_type, &input_payload, &input_payload_size))
                {
                    close(input_tcp_fd);
                    input_tcp_fd = -1;

                    pthread_mutex_lock(&net->lock);
                    if (net->input_tcp_fd >= 0)
                    {
                        net->input_tcp_fd = -1;
                        net->stats.tcp_input_channel_closed++;
                    }
                    pthread_mutex_unlock(&net->lock);
                }
                else
                {
                    pthread_mutex_lock(&net->lock);
                    net->stats.tcp_input_channel_rx++;
                    pthread_mutex_unlock(&net->lock);

                    if (input_type == WD_MSG_KEYBOARD_KEY)
                    {
                        wd_server_handle_keyboard_message(server, &cfg, input_payload, input_payload_size);
                    }
                    else if (input_type == WD_MSG_POINTER_EVENT)
                    {
                        wd_server_handle_pointer_message(server, &cfg, input_payload, input_payload_size);
                    }

                    free(input_payload);
                }
            }

            if (have_select_pfd && (pfds[select_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            {
                uint16_t selection_type         = 0;
                uint8_t* selection_payload      = NULL;
                uint32_t selection_payload_size = 0;

                if (!wd_recv_tcp_message(selection_tcp_fd, &selection_type, &selection_payload, &selection_payload_size))
                {
                    close(selection_tcp_fd);
                    selection_tcp_fd = -1;

                    pthread_mutex_lock(&net->lock);
                    if (net->selection_tcp_fd >= 0)
                    {
                        net->selection_tcp_fd = -1;
                        net->stats.tcp_selection_channel_closed++;
                    }
                    pthread_mutex_unlock(&net->lock);
                }
                else
                {
                    if ((selection_type == WD_MSG_CLIPBOARD_SET || selection_type == WD_MSG_PRIMARY_SET) &&
                        selection_payload_size >= sizeof(struct wd_selection_payload_header))
                    {
                        pthread_mutex_lock(&net->lock);
                        net->stats.tcp_selection_channel_rx++;
                        wd_clipboard_queue_client_set_locked(net, cfg.session_id, selection_payload, selection_payload_size,
                                                             selection_type == WD_MSG_PRIMARY_SET);
                        pthread_mutex_unlock(&net->lock);
                    }

                    free(selection_payload);
                }
            }

            if (pfds[control_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
            {
                payload      = NULL;
                payload_size = 0;

                if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size))
                {
                    break;
                }

                if (type == WD_MSG_RETRANSMIT_REQUEST && payload_size >= sizeof(struct wd_retransmit_request_payload_header))
                {
                    struct wd_retransmit_request_payload_header rh;
                    memcpy(&rh, payload, sizeof(rh));

                    size_t needed = sizeof(rh) + (size_t)rh.request_count * sizeof(struct wd_retransmit_entry);

                    if (rh.session_id == cfg.session_id && payload_size >= needed)
                    {
                        struct wd_retransmit_entry* entries = (struct wd_retransmit_entry*)(payload + sizeof(rh));

                        pthread_mutex_lock(&net->lock);

                        net->stats.retx_req_rx++;

                        uint64_t accepted_retransmits = 0;

                        for (uint16_t i = 0; i < rh.request_count; ++i)
                        {
                            if (entries[i].tile_id >= server->total_tiles)
                            {
                                continue;
                            }

                            struct wd_tile_state* tile = &net->tiles[entries[i].tile_id];
                            uint64_t requested_generation = entries[i].requested_generation;


                            if (requested_generation != 0 && tile->generation < requested_generation)
                            {
                                net->stats.retx_req_waiting_for_generation++;
                                continue;
                            }

                            if (requested_generation != 0 && tile->generation > requested_generation)
                            {
                                net->stats.retx_req_stale_generation++;
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
                else if (type == WD_MSG_CLIENT_STATS && payload_size >= sizeof(struct wd_client_stats_payload))
                {
                    struct wd_client_stats_payload cs;
                    memcpy(&cs, payload, sizeof(cs));

                    if (cs.session_id == cfg.session_id)
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
                        if (cs.udp_interarrival_max_ns > net->stats.client_udp_interarrival_max_ns)
                        {
                            net->stats.client_udp_interarrival_max_ns = cs.udp_interarrival_max_ns;
                        }
                        pthread_mutex_unlock(&net->lock);
                    }
                }
                else if ((type == WD_MSG_CLIPBOARD_SET || type == WD_MSG_PRIMARY_SET) &&
                         payload_size >= sizeof(struct wd_selection_payload_header))
                {
                    pthread_mutex_lock(&net->lock);
                    wd_clipboard_queue_client_set_locked(net, cfg.session_id, payload, payload_size, type == WD_MSG_PRIMARY_SET);
                    pthread_mutex_unlock(&net->lock);
                }
                else if (type == WD_MSG_DISPLAY_RESIZE && payload_size >= sizeof(struct wd_display_resize_payload))
                {
                    struct wd_display_resize_payload resize;
                    memcpy(&resize, payload, sizeof(resize));

                    if (resize.session_id == cfg.session_id && resize.width != 0 && resize.height != 0)
                    {
                        if (wd_server_request_display_size(server, resize.width, resize.height))
                        {
                            wd_server_fill_config(server, cfg.session_id, selected_udp_payload, &cfg);

                            if (!wd_send_tcp_message(tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg)))
                            {
                                free(payload);
                                break;
                            }

                            WD_LOG_INFO("WayDisplay: client resized display to %ux%u", server->display_width, server->display_height);
                        }
                        else
                        {
                            WD_LOG_ERROR("WayDisplay: rejected display resize to %ux%u", resize.width, resize.height);
                        }
                    }
                }
                else if (type == WD_MSG_KEYBOARD_KEY)
                {
                    wd_server_handle_keyboard_message(server, &cfg, payload, payload_size);
                }
                else if (type == WD_MSG_POINTER_EVENT)
                {
                    wd_server_handle_pointer_message(server, &cfg, payload, payload_size);
                }

                free(payload);
            }
        }

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

        net->client_connected    = false;
        net->key_queue_count     = 0;
        net->pointer_queue_count = 0;
        net->key_state_reset_pending = true;

        free(net->clipboard_text);
        net->clipboard_text         = NULL;
        net->clipboard_text_size    = 0;
        net->clipboard_text_pending = false;

        free(net->primary_text);
        net->primary_text         = NULL;
        net->primary_text_size    = 0;
        net->primary_text_pending = false;

        pthread_mutex_unlock(&net->lock);

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

        WD_LOG_INFO("WayDisplay: client disconnected; waiting for reconnect");
    }

    wd_close_fd(&net->tcp_fd);
    wd_close_fd(&net->input_tcp_fd);
    wd_close_fd(&net->selection_tcp_fd);
    wd_close_fd(&net->udp_fd);
    wd_close_fd(&net->listen_fd);

    return NULL;
}
