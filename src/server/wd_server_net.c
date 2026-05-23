#include "wd_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "waydisplay/wd_net.h"
#include "waydisplay/wd_time.h"

bool wd_net_init(struct wd_server *server, uint16_t tcp_port) {
  struct wd_net_state *net = &server->net;

  memset(net, 0, sizeof(*net));

  if (pthread_mutex_init(&net->lock, NULL) != 0) {
    return false;
  }

  net->running = true;
  net->tcp_port = tcp_port;
  net->tcp_fd = -1;
  net->listen_fd = -1;
  net->udp_fd = -1;
  net->session_id = 0;
  net->full_frame_needed = true;
  net->full_frame_next_tile = 0;
  net->dirty_scan_next_tile = 0;
  net->udp_payload_target = WD_UDP_PAYLOAD_TARGET;

  wd_stream_policy_set_defaults(&net->stream_policy);

  return true;
}

void wd_net_destroy(struct wd_server *server) {
  struct wd_net_state *net = &server->net;

  net->running = false;

  if (net->listen_fd >= 0) {
    close(net->listen_fd);
    net->listen_fd = -1;
  }

  if (net->tcp_fd >= 0) {
    close(net->tcp_fd);
    net->tcp_fd = -1;
  }

  if (net->udp_fd >= 0) {
    close(net->udp_fd);
    net->udp_fd = -1;
  }

  pthread_mutex_destroy(&net->lock);
}

static uint16_t run_udp_mtu_probe(struct wd_server *server,
                                  int tcp_fd,
                                  const struct sockaddr_in *client_udp_addr) {
  struct wd_net_state *net = &server->net;

  /*
   * Payload size excluding wd_udp_tile_packet_header.
   *
   * 65487 = max IPv4 UDP payload 65507 - WayDisplay UDP tile header 20.
   * This should work on loopback if the OS accepts max-size UDP datagrams.
   */
  static const uint16_t probe_sizes[] = {
    65487,
    8192,
    4096,
    1450,
    1400,
    1360,
    1300,
    1200,
  };

  const uint16_t probe_count =
  (uint16_t)(sizeof(probe_sizes) / sizeof(probe_sizes[0]));

  struct wd_mtu_probe_start_payload start;
  memset(&start, 0, sizeof(start));
  start.session_id = net->session_id;
  start.probe_count = probe_count;

  if (!wd_send_tcp_message(tcp_fd,
    WD_MSG_MTU_PROBE_START,
    &start,
    sizeof(start))) {
    return WD_UDP_PAYLOAD_TARGET;
    }

    /*
     * Give the client a moment to enter its UDP-probe receive loop.
     */
    wd_sleep_ms(10);

  uint8_t packet[sizeof(struct wd_udp_tile_packet_header) + 65487];

  for (uint16_t i = 0; i < probe_count; ++i) {
    uint16_t payload_size = probe_sizes[i];

    memset(packet, 0, sizeof(packet));

    struct wd_udp_tile_packet_header *h =
    (struct wd_udp_tile_packet_header *)packet;

    h->tile_id = WD_UDP_TILE_ID_MTU_PROBE;
    h->tile_pkt_count = probe_count;
    h->tile_pkt_id = i;
    h->payload_size = payload_size;
    h->tile_generation = net->session_id;
    h->compressed_tile_size = payload_size;

    memset(packet + sizeof(*h), 0xa5, payload_size);

    ssize_t sent =
    sendto(net->udp_fd,
           packet,
           sizeof(*h) + payload_size,
           0,
           (const struct sockaddr *)client_udp_addr,
           sizeof(*client_udp_addr));

    if (sent < 0) {
      /*
       * Expected for jumbo probes on non-jumbo paths. Keep trying
       * smaller probes.
       */
      continue;
    }
  }

  uint16_t type = 0;
  uint8_t *payload = NULL;
  uint32_t payload_size = 0;

  if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size)) {
    free(payload);
    return WD_UDP_PAYLOAD_TARGET;
  }

  uint16_t result = WD_UDP_PAYLOAD_TARGET;

  if (type == WD_MSG_MTU_PROBE_RESULT &&
    payload_size >= sizeof(struct wd_mtu_probe_result_payload)) {
    struct wd_mtu_probe_result_payload probe_result;
  memcpy(&probe_result, payload, sizeof(probe_result));

  if (probe_result.session_id == net->session_id &&
    probe_result.max_udp_payload_received >= 512) {
    result = probe_result.max_udp_payload_received;
    }
    }

    free(payload);

  if (result > 65487) {
    result = 65487;
  }

  if (result < 512) {
    result = WD_UDP_PAYLOAD_TARGET;
  }

  wlr_log(WLR_INFO,
          "WayDisplay: UDP payload target selected by probe: %u",
          result);

  return result;
                                  }


void *wd_net_thread_main(void *arg) {
  struct wd_server *server = arg;
  struct wd_net_state *net = &server->net;

  net->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (net->listen_fd < 0) {
    wlr_log(WLR_ERROR, "WayDisplay: TCP socket failed: %s", strerror(errno));
    return NULL;
  }

  int yes = 1;
  setsockopt(net->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));

  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;
  bind_addr.sin_port = htons(net->tcp_port);

  if (bind(net->listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) <
      0) {
    wlr_log(WLR_ERROR, "WayDisplay: bind TCP failed: %s", strerror(errno));
    return NULL;
  }

  if (listen(net->listen_fd, 1) < 0) {
    wlr_log(WLR_ERROR, "WayDisplay: listen failed: %s", strerror(errno));
    return NULL;
  }

  net->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (net->udp_fd < 0) {
    wlr_log(WLR_ERROR, "WayDisplay: UDP socket failed: %s", strerror(errno));
    return NULL;
  }

  wlr_log(WLR_INFO, "WayDisplay: network server listening on TCP port %u",
          net->tcp_port);

  while (net->running) {
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    int tcp_fd =
        accept(net->listen_fd, (struct sockaddr *)&peer_addr, &peer_len);

    if (tcp_fd < 0) {
      if (!net->running) {
        break;
      }

      if (errno == EINTR) {
        continue;
      }

      wlr_log(WLR_ERROR, "WayDisplay: accept failed: %s", strerror(errno));
      continue;
    }

    uint16_t type = 0;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0;

    if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size) ||
        type != WD_MSG_CLIENT_HELLO ||
        payload_size < sizeof(struct wd_client_hello_payload)) {
      wlr_log(WLR_ERROR, "WayDisplay: invalid client hello");
      free(payload);
      close(tcp_fd);
      continue;
    }

    struct wd_client_hello_payload hello;
    memcpy(&hello, payload, sizeof(hello));
    free(payload);
    payload = NULL;

    struct wd_server_config_payload cfg;
    memset(&cfg, 0, sizeof(cfg));

    pthread_mutex_lock(&net->lock);

    if (net->session_id == 0) {
      net->session_id = (uint32_t)(wd_now_ns() ^ 0x9e3779b9u);
    }

    cfg.session_id = net->session_id;

    pthread_mutex_unlock(&net->lock);

    cfg.width = WD_DISPLAY_WIDTH;
    cfg.height = WD_DISPLAY_HEIGHT;
    cfg.tile_width = WD_TILE_WIDTH;
    cfg.tile_height = WD_TILE_HEIGHT;
    cfg.tiles_x = WD_TILES_X;
    cfg.tiles_y = WD_TILES_Y;
    cfg.total_tiles = WD_TOTAL_TILES;
    cfg.pixel_format = WD_PIXEL_FORMAT_XRGB8888;
    cfg.compression_mode = WD_COMPRESSION_ZSTD;
    cfg.zstd_level = WD_ZSTD_LEVEL;

    struct sockaddr_in client_udp_addr;
    memset(&client_udp_addr, 0, sizeof(client_udp_addr));

    client_udp_addr.sin_family = AF_INET;
    client_udp_addr.sin_addr = peer_addr.sin_addr;
    client_udp_addr.sin_port = htons(hello.client_udp_port);

    uint16_t selected_udp_payload =
    run_udp_mtu_probe(server, tcp_fd, &client_udp_addr);

    pthread_mutex_lock(&net->lock);
    net->udp_payload_target = selected_udp_payload;
    pthread_mutex_unlock(&net->lock);

    cfg.udp_payload_target = selected_udp_payload;

    if (!wd_send_tcp_message(tcp_fd, WD_MSG_SERVER_CONFIG, &cfg, sizeof(cfg))) {
      wlr_log(WLR_ERROR, "WayDisplay: failed to send server config");
      close(tcp_fd);
      continue;
    }

    pthread_mutex_lock(&net->lock);

    wd_stream_policy_apply_client_hello(&net->stream_policy, &hello);

    net->tcp_fd = tcp_fd;
    net->client_udp_addr = client_udp_addr;
    net->client_connected = true;
    net->full_frame_needed = true;
    net->full_frame_next_tile = 0;
    net->dirty_scan_next_tile = 0;

    net->stats.tcp_hello_rx++;
    net->stats.tcp_config_tx++;

    net->key_queue_count = 0;
    net->pointer_queue_count = 0;

    pthread_mutex_unlock(&net->lock);

    wlr_log(WLR_INFO,
            "WayDisplay: client connected; UDP port=%u stream_mode=%u fps=%u "
            "max_tiles_per_sec=%u",
            hello.client_udp_port, hello.stream_mode, hello.target_fps,
            hello.max_tiles_per_second);

    while (net->running) {
      payload = NULL;
      payload_size = 0;

      if (!wd_recv_tcp_message(tcp_fd, &type, &payload, &payload_size)) {
        break;
      }

      if (type == WD_MSG_RETRANSMIT_REQUEST &&
          payload_size >= sizeof(struct wd_retransmit_request_payload_header) && net->stream_policy.mode != WD_STREAM_MODE_LIVE) {
        struct wd_retransmit_request_payload_header rh;
        memcpy(&rh, payload, sizeof(rh));

        size_t needed = sizeof(rh) + (size_t)rh.request_count *
                                         sizeof(struct wd_retransmit_entry);

        if (rh.session_id == cfg.session_id && payload_size >= needed) {
          struct wd_retransmit_entry *entries =
              (struct wd_retransmit_entry *)(payload + sizeof(rh));

          pthread_mutex_lock(&net->lock);

          if (net->full_frame_needed) {
              /*
               * During progressive initial frame delivery, the client will naturally
               * be missing tiles we have not reached yet. Do not spend bandwidth
               * retransmitting early tiles until the first full sweep completes.
               */
              pthread_mutex_unlock(&net->lock);
              free(payload);
              continue;
          }
          net->stats.retx_req_rx++;
          net->stats.retx_tiles_req += rh.request_count;

          for (uint16_t i = 0; i < rh.request_count; ++i) {
            if (entries[i].tile_id < WD_TOTAL_TILES) {
              wd_stream_send_cached_tile_locked(server, entries[i].tile_id);
            }
          }

          pthread_mutex_unlock(&net->lock);
        }
      } else if (type == WD_MSG_KEYBOARD_KEY &&
                 payload_size >= sizeof(struct wd_keyboard_event_payload)) {
        struct wd_keyboard_event_payload key;
        memcpy(&key, payload, sizeof(key));

        if (key.session_id == cfg.session_id && key.evdev_key_code != 0) {
          pthread_mutex_lock(&net->lock);
          wd_keyboard_queue_event_locked(net, &key);
          pthread_mutex_unlock(&net->lock);
        }
      } else if (type == WD_MSG_POINTER_EVENT &&
                 payload_size >= sizeof(struct wd_pointer_event_payload)) {
        struct wd_pointer_event_payload pointer;
        memcpy(&pointer, payload, sizeof(pointer));

        if (pointer.session_id == cfg.session_id) {
          pthread_mutex_lock(&net->lock);
          wd_pointer_queue_event_locked(net, &pointer);
          pthread_mutex_unlock(&net->lock);
        }
      }

      free(payload);
    }

    pthread_mutex_lock(&net->lock);

    if (net->tcp_fd == tcp_fd) {
      net->tcp_fd = -1;
    }

    net->client_connected = false;
    net->key_queue_count = 0;
    net->pointer_queue_count = 0;

    pthread_mutex_unlock(&net->lock);

    close(tcp_fd);

    wlr_log(WLR_INFO, "WayDisplay: client disconnected; waiting for reconnect");
  }

  return NULL;
}
