#include "waydisplay/wd_net.h"
#include "waydisplay/wd_time.h"
#include "wd_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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

    net->running              = true;
    net->tcp_port             = tcp_port;
    net->tcp_fd               = -1;
    net->listen_fd            = -1;
    net->udp_fd               = -1;
    net->session_id           = 0;
    net->full_frame_needed    = true;
    net->full_frame_next_tile = 0;
    net->dirty_scan_next_tile = 0;
    net->dirty_queue       = calloc(server->total_tiles, sizeof(*net->dirty_queue));
    net->dirty_queued      = calloc(server->total_tiles, sizeof(*net->dirty_queued));
    net->retransmit_queue  = calloc(server->total_tiles, sizeof(*net->retransmit_queue));
    net->retransmit_queued = calloc(server->total_tiles, sizeof(*net->retransmit_queued));
    if (!net->dirty_queue || !net->dirty_queued || !net->retransmit_queue || !net->retransmit_queued)
    {
        free(net->dirty_queue);
        free(net->dirty_queued);
        free(net->retransmit_queue);
        free(net->retransmit_queued);
        net->dirty_queue        = NULL;
        net->dirty_queued       = NULL;
        net->retransmit_queue   = NULL;
        net->retransmit_queued  = NULL;
        pthread_mutex_destroy(&net->lock);
        return false;
    }
    net->dirty_queue_read       = 0;
    net->dirty_queue_write      = 0;
    net->dirty_queue_count      = 0;
    net->retransmit_queue_count = 0;
    net->tile_queue_rng_state   = (uint32_t)wd_now_ns() | 1u;
    net->udp_payload_target = WD_UDP_PAYLOAD_TARGET;

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

    if (net->udp_fd >= 0)
    {
        close(net->udp_fd);
        net->udp_fd = -1;
    }

    free(net->dirty_queue);
    net->dirty_queue = NULL;

    free(net->dirty_queued);
    net->dirty_queued = NULL;

    free(net->retransmit_queue);
    net->retransmit_queue = NULL;

    free(net->retransmit_queued);
    net->retransmit_queued = NULL;

    free(net->clipboard_text);
    net->clipboard_text         = NULL;
    net->clipboard_text_size    = 0;
    net->clipboard_text_pending = false;

    free(net->primary_text);
    net->primary_text         = NULL;
    net->primary_text_size    = 0;
    net->primary_text_pending = false;

    pthread_mutex_destroy(&net->lock);
}

struct wd_udp_mtu_probe_df_state {
    bool supported;
    bool have_old_value;
    int  old_value;
};

static struct wd_udp_mtu_probe_df_state wd_udp_mtu_probe_enable_df(int udp_fd) {
    struct wd_udp_mtu_probe_df_state state;
    memset(&state, 0, sizeof(state));

#if defined(IP_MTU_DISCOVER)
    socklen_t old_len = sizeof(state.old_value);
    if (getsockopt(udp_fd, IPPROTO_IP, IP_MTU_DISCOVER, &state.old_value, &old_len) == 0)
    {
        state.have_old_value = true;
    }

#    if defined(IP_PMTUDISC_PROBE)
    const int mode = IP_PMTUDISC_PROBE;
#    elif defined(IP_PMTUDISC_DO)
    const int mode = IP_PMTUDISC_DO;
#    else
    const int mode = -1;
#    endif

    if (mode >= 0 && setsockopt(udp_fd, IPPROTO_IP, IP_MTU_DISCOVER, &mode, sizeof(mode)) == 0)
    {
        state.supported = true;
    }
#else
    (void)udp_fd;
#endif

    return state;
}

static void wd_udp_mtu_probe_restore_df(int udp_fd, const struct wd_udp_mtu_probe_df_state* state) {
#if defined(IP_MTU_DISCOVER)
    if (state && state->have_old_value)
    {
        (void)setsockopt(udp_fd, IPPROTO_IP, IP_MTU_DISCOVER, &state->old_value, sizeof(state->old_value));
        return;
    }

#    if defined(IP_PMTUDISC_WANT)
    const int mode = IP_PMTUDISC_WANT;
    (void)setsockopt(udp_fd, IPPROTO_IP, IP_MTU_DISCOVER, &mode, sizeof(mode));
#    endif
#else
    (void)udp_fd;
    (void)state;
#endif
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

    struct wd_udp_mtu_probe_df_state df_state = wd_udp_mtu_probe_enable_df(net->udp_fd);
    if (!df_state.supported)
    {
        WD_LOG_INFO("WayDisplay: UDP MTU probe could not force DF; using default UDP payload target: %u", WD_UDP_PAYLOAD_TARGET);
        return WD_UDP_PAYLOAD_TARGET;
    }

    /*
     * Give the client a moment to enter its UDP-probe receive loop.
     * Probe packets are sent with IPv4 DF set so successful receipt means
     * the datagram fit the path MTU instead of being IP-fragmented and
     * reassembled.
     */
    wd_sleep_ms(10);

    uint8_t packet[sizeof(struct wd_udp_tile_packet_header) + WD_UDP_TILE_PAYLOAD_MAX];

    for (uint16_t i = 0; i < probe_count; ++i)
    {
        uint16_t payload_size = probe_sizes[i];
        size_t   packet_size  = sizeof(struct wd_udp_tile_packet_header) + payload_size;

        memset(packet, 0, sizeof(packet));

        struct wd_udp_tile_packet_header* h = (struct wd_udp_tile_packet_header*)packet;

        h->tile_id              = WD_UDP_TILE_ID_MTU_PROBE;
        h->tile_pkt_count       = probe_count;
        h->tile_pkt_id          = i;
        h->payload_size         = payload_size;
        h->tile_generation      = net->session_id;
        h->compressed_tile_size = payload_size;

        memset(packet + sizeof(*h), 0xa5, payload_size);

        ssize_t sent = sendto(net->udp_fd, packet, packet_size, 0, (const struct sockaddr*)client_udp_addr, sizeof(*client_udp_addr));

        if (sent < 0 || (size_t)sent != packet_size)
        {
            /*
             * EMSGSIZE is expected for probes above the path/interface MTU
             * when DF is set. Keep trying smaller probes.
             */
            continue;
        }
    }

    wd_udp_mtu_probe_restore_df(net->udp_fd, &df_state);

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

static void wd_server_fill_config(struct wd_server* server, uint32_t session_id, uint16_t udp_payload_target,
                                  struct wd_server_config_payload* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->session_id         = session_id;
    cfg->width              = (uint16_t)server->display_width;
    cfg->height             = (uint16_t)server->display_height;
    cfg->tile_width         = WD_TILE_WIDTH;
    cfg->tile_height        = WD_TILE_HEIGHT;
    cfg->tiles_x            = server->tiles_x;
    cfg->tiles_y            = server->tiles_y;
    cfg->total_tiles        = server->total_tiles;
    cfg->pixel_format       = WD_PIXEL_FORMAT_XRGB8888;
    cfg->compression_mode   = WD_COMPRESSION_ZSTD;
    cfg->zstd_level         = WD_ZSTD_LEVEL;
    cfg->udp_payload_target = udp_payload_target;
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

    if (listen(net->listen_fd, 1) < 0)
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

    if (wd_set_nonblocking(net->udp_fd) < 0)
    {
        WD_LOG_ERROR("WayDisplay: failed to make UDP socket nonblocking: %s", strerror(errno));
    }

    WD_LOG_INFO("WayDisplay: network server listening on TCP port %u", net->tcp_port);

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
        uint32_t                        session_id = 0;

        pthread_mutex_lock(&net->lock);

        if (net->session_id == 0)
        {
            net->session_id = (uint32_t)(wd_now_ns() ^ 0x9e3779b9u);
        }

        session_id = net->session_id;

        pthread_mutex_unlock(&net->lock);

        struct sockaddr_in client_udp_addr;
        memset(&client_udp_addr, 0, sizeof(client_udp_addr));

        client_udp_addr.sin_family = AF_INET;
        client_udp_addr.sin_addr   = peer_addr.sin_addr;
        client_udp_addr.sin_port   = htons(hello.client_udp_port);

        uint16_t selected_udp_payload = run_udp_mtu_probe(server, tcp_fd, &client_udp_addr);

        pthread_mutex_lock(&net->lock);
        net->udp_payload_target = selected_udp_payload;
        pthread_mutex_unlock(&net->lock);

        wd_server_fill_config(server, session_id, selected_udp_payload, &cfg);

        if (!wd_send_tcp_message(tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg)))
        {
            WD_LOG_ERROR("WayDisplay: failed to send server config");
            close(tcp_fd);
            continue;
        }

        wd_clear_tcp_receive_timeout(tcp_fd);

        pthread_mutex_lock(&net->lock);

        wd_stream_policy_apply_client_hello(&net->stream_policy, &hello);

        net->tcp_fd               = tcp_fd;
        net->client_udp_addr      = client_udp_addr;
        net->client_connected     = true;
        net->full_frame_needed    = true;
        net->full_frame_next_tile = 0;
        net->dirty_scan_next_tile = 0;
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

        net->stats.tcp_hello_rx++;
        net->stats.tcp_config_tx++;

        net->key_queue_count     = 0;
        net->pointer_queue_count = 0;

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

        WD_LOG_INFO("WayDisplay: client connected; UDP port=%u display=%ux%u stream_mode=%u fps=%u "
                    "max_tiles_per_sec=%u retx_tiles_per_sec=%u",
                    hello.client_udp_port, server->display_width, server->display_height, hello.stream_mode, hello.target_fps,
                    hello.max_tiles_per_second, net->stream_policy.max_retransmit_tiles_per_second);

        while (net->running)
        {
            payload      = NULL;
            payload_size = 0;

            if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size))
            {
                break;
            }

            if (type == WD_MSG_RETRANSMIT_REQUEST && payload_size >= sizeof(struct wd_retransmit_request_payload_header) &&
                net->stream_policy.mode != WD_STREAM_MODE_LIVE)
            {
                struct wd_retransmit_request_payload_header rh;
                memcpy(&rh, payload, sizeof(rh));

                size_t needed = sizeof(rh) + (size_t)rh.request_count * sizeof(struct wd_retransmit_entry);

                if (rh.session_id == cfg.session_id && payload_size >= needed)
                {
                    struct wd_retransmit_entry* entries = (struct wd_retransmit_entry*)(payload + sizeof(rh));

                    pthread_mutex_lock(&net->lock);

                    net->stats.retx_req_rx++;
                    net->stats.retx_tiles_req += rh.request_count;

                    if (!net->full_frame_needed)
                    {
                        for (uint16_t i = 0; i < rh.request_count; ++i)
                        {
                            if (entries[i].tile_id < server->total_tiles)
                            {
                                wd_stream_queue_retransmit_tile_locked(server, entries[i].tile_id);
                            }
                        }
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
                    if (wd_server_apply_display_size(server, resize.width, resize.height))
                    {
                        wd_server_fill_config(server, cfg.session_id, selected_udp_payload, &cfg);

                        if (!wd_send_tcp_message(tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg)))
                        {
                            break;
                        }

                        WD_LOG_INFO("WayDisplay: client resized display to %ux%u", server->display_width, server->display_height);
                    }
                    else
                    {
                        WD_LOG_ERROR("WayDisplay: rejected live display resize to %ux%u", resize.width, resize.height);
                    }
                }
            }
            else if (type == WD_MSG_KEYBOARD_KEY && payload_size >= sizeof(struct wd_keyboard_event_payload))
            {
                struct wd_keyboard_event_payload key;
                memcpy(&key, payload, sizeof(key));

                if (key.session_id == cfg.session_id && key.evdev_key_code != 0)
                {
                    pthread_mutex_lock(&net->lock);
                    wd_keyboard_queue_event_locked(net, &key, wd_now_ns());
                    pthread_mutex_unlock(&net->lock);
                }
            }
            else if (type == WD_MSG_POINTER_EVENT && payload_size >= sizeof(struct wd_pointer_event_payload))
            {
                struct wd_pointer_event_payload pointer;
                memcpy(&pointer, payload, sizeof(pointer));

                if (pointer.session_id == cfg.session_id)
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

            free(payload);
        }

        pthread_mutex_lock(&net->lock);

        if (net->tcp_fd == tcp_fd)
        {
            net->tcp_fd = -1;
        }

        net->client_connected    = false;
        net->key_queue_count     = 0;
        net->pointer_queue_count = 0;

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

        WD_LOG_INFO("WayDisplay: client disconnected; waiting for reconnect");
    }

    wd_close_fd(&net->tcp_fd);
    wd_close_fd(&net->udp_fd);
    wd_close_fd(&net->listen_fd);

    return NULL;
}
