#include "wd_server.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

#include <wlr/types/wlr_primary_selection.h>

#include "waydisplay/wd_time.h"

#define WD_PASTE_MAX_TEXT_BYTES (64u * 1024u)
#define WD_PASTE_CHARS_PER_TICK 4u

struct wd_active_paste {
  uint8_t *text;
  uint32_t size;
  uint32_t offset;
  bool primary;
};

static struct wd_active_paste g_active_paste;

static void handle_request_set_selection(struct wl_listener *listener,
                                         void *data) {
  struct wd_server *server =
      wl_container_of(listener, server, request_set_selection);
  struct wlr_seat_request_set_selection_event *event = data;

  wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener,
                                                 void *data) {
  struct wd_server *server =
      wl_container_of(listener, server, request_set_primary_selection);
  struct wlr_seat_request_set_primary_selection_event *event = data;

  wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void remove_listener_if_linked(struct wl_listener *listener) {
  if (!listener) {
    return;
  }

  if (listener->link.prev && listener->link.next) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
  }
}

bool wd_clipboard_init(struct wd_server *server) {
  if (!server || !server->display || !server->seat) {
    return false;
  }

  server->data_device_manager =
      wlr_data_device_manager_create(server->display);

  if (!server->data_device_manager) {
    wlr_log(WLR_ERROR, "WayDisplay: failed to create data device manager");
    return false;
  }

  server->primary_selection_manager =
      wlr_primary_selection_v1_device_manager_create(server->display);

  if (!server->primary_selection_manager) {
    wlr_log(WLR_ERROR,
            "WayDisplay: failed to create primary selection manager");
    return false;
  }

  server->request_set_selection.notify = handle_request_set_selection;
  wl_signal_add(&server->seat->events.request_set_selection,
                &server->request_set_selection);

  server->request_set_primary_selection.notify =
      handle_request_set_primary_selection;
  wl_signal_add(&server->seat->events.request_set_primary_selection,
                &server->request_set_primary_selection);

  return true;
}

void wd_clipboard_destroy(struct wd_server *server) {
  if (!server) {
    return;
  }

  remove_listener_if_linked(&server->request_set_selection);
  remove_listener_if_linked(&server->request_set_primary_selection);
}

static bool payload_to_text_copy(uint32_t expected_session_id,
                                 const uint8_t *payload,
                                 uint32_t payload_size,
                                 uint8_t **out_text,
                                 uint32_t *out_text_size) {
  if (out_text) {
    *out_text = NULL;
  }

  if (out_text_size) {
    *out_text_size = 0;
  }

  if (!payload || !out_text || !out_text_size ||
      payload_size < sizeof(struct wd_selection_payload_header)) {
    return false;
  }

  struct wd_selection_payload_header header;
  memcpy(&header, payload, sizeof(header));

  if (header.session_id != expected_session_id ||
      (header.mime_type != WD_SELECTION_MIME_TEXT_UTF8 &&
       header.mime_type != WD_SELECTION_MIME_TEXT_PLAIN) ||
      header.data_size > WD_PASTE_MAX_TEXT_BYTES) {
    return false;
  }

  size_t needed = sizeof(header) + (size_t)header.data_size;
  if (payload_size < needed) {
    return false;
  }

  uint8_t *text = calloc((size_t)header.data_size + 1u, 1);
  if (!text) {
    return false;
  }

  if (header.data_size > 0) {
    memcpy(text, payload + sizeof(header), header.data_size);
  }

  *out_text = text;
  *out_text_size = header.data_size;
  return true;
}

void wd_clipboard_queue_client_set_locked(struct wd_net_state *net,
                                          uint32_t expected_session_id,
                                          const uint8_t *payload,
                                          uint32_t payload_size,
                                          bool primary) {
  if (!net) {
    return;
  }

  uint8_t *text = NULL;
  uint32_t text_size = 0;

  if (!payload_to_text_copy(expected_session_id,
                            payload,
                            payload_size,
                            &text,
                            &text_size)) {
    return;
  }

  if (primary) {
    free(net->primary_text);
    net->primary_text = text;
    net->primary_text_size = text_size;
    net->primary_text_pending = true;
  } else {
    free(net->clipboard_text);
    net->clipboard_text = text;
    net->clipboard_text_size = text_size;
    net->clipboard_text_pending = true;
  }
}

struct wd_key_for_char {
  uint16_t key;
  bool shift;
};

static bool key_for_ascii(unsigned char ch, struct wd_key_for_char *out) {
  if (!out) {
    return false;
  }

  memset(out, 0, sizeof(*out));

  if (ch >= 'a' && ch <= 'z') {
    static const uint16_t keys[] = {
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    };
    out->key = keys[ch - 'a'];
    return true;
  }

  if (ch >= 'A' && ch <= 'Z') {
    if (!key_for_ascii((unsigned char)(ch - 'A' + 'a'), out)) {
      return false;
    }
    out->shift = true;
    return true;
  }

  if (ch >= '1' && ch <= '9') {
    out->key = (uint16_t)(ch - '1' + 2);
    return true;
  }

  switch (ch) {
    case '0': out->key = 11; return true;
    case '\n': out->key = 28; return true;
    case '\r': out->key = 28; return true;
    case '\t': out->key = 15; return true;
    case ' ': out->key = 57; return true;

    case '-': out->key = 12; return true;
    case '_': out->key = 12; out->shift = true; return true;
    case '=': out->key = 13; return true;
    case '+': out->key = 13; out->shift = true; return true;
    case '[': out->key = 26; return true;
    case '{': out->key = 26; out->shift = true; return true;
    case ']': out->key = 27; return true;
    case '}': out->key = 27; out->shift = true; return true;
    case ';': out->key = 39; return true;
    case ':': out->key = 39; out->shift = true; return true;
    case '\'': out->key = 40; return true;
    case '"': out->key = 40; out->shift = true; return true;
    case '`': out->key = 41; return true;
    case '~': out->key = 41; out->shift = true; return true;
    case '\\': out->key = 43; return true;
    case '|': out->key = 43; out->shift = true; return true;
    case ',': out->key = 51; return true;
    case '<': out->key = 51; out->shift = true; return true;
    case '.': out->key = 52; return true;
    case '>': out->key = 52; out->shift = true; return true;
    case '/': out->key = 53; return true;
    case '?': out->key = 53; out->shift = true; return true;

    case '!': out->key = 2; out->shift = true; return true;
    case '@': out->key = 3; out->shift = true; return true;
    case '#': out->key = 4; out->shift = true; return true;
    case '$': out->key = 5; out->shift = true; return true;
    case '%': out->key = 6; out->shift = true; return true;
    case '^': out->key = 7; out->shift = true; return true;
    case '&': out->key = 8; out->shift = true; return true;
    case '*': out->key = 9; out->shift = true; return true;
    case '(': out->key = 10; out->shift = true; return true;
    case ')': out->key = 11; out->shift = true; return true;
    default: return false;
  }
}

static void inject_key(struct wd_server *server, uint16_t key, bool pressed) {
  enum wl_keyboard_key_state state = pressed
      ? WL_KEYBOARD_KEY_STATE_PRESSED
      : WL_KEYBOARD_KEY_STATE_RELEASED;

  wlr_seat_keyboard_notify_key(server->seat,
                               wd_now_ms32(),
                               key,
                               state);

  pthread_mutex_lock(&server->net.lock);
  server->net.stats.key_events_injected++;
  pthread_mutex_unlock(&server->net.lock);
}

static uint32_t inject_text_as_keys_limited(struct wd_server *server,
                                        const uint8_t *text,
                                        uint32_t text_size,
                                        bool primary,
                                        uint32_t max_chars) {
  if (!server || !server->seat || !server->keyboard || !text || max_chars == 0) {
    return 0;
  }

  wlr_seat_set_keyboard(server->seat, server->keyboard);

  if (server->focused_surface) {
    wlr_seat_keyboard_notify_enter(server->seat,
                                   server->focused_surface,
                                   server->keyboard->keycodes,
                                   server->keyboard->num_keycodes,
                                   &server->keyboard->modifiers);
  }

  uint32_t consumed = 0;
  uint32_t injected = 0;
  uint32_t skipped = 0;

  while (consumed < text_size && injected < max_chars) {
    struct wd_key_for_char key;
    unsigned char ch = text[consumed++];

    if (!key_for_ascii(ch, &key)) {
      skipped++;
      continue;
    }

    if (key.shift) {
      inject_key(server, 42, true);
    }

    inject_key(server, key.key, true);
    inject_key(server, key.key, false);

    if (key.shift) {
      inject_key(server, 42, false);
    }

    injected++;
  }

  if (skipped > 0) {
    wlr_log(WLR_INFO,
            "WayDisplay: pasted %u %s characters this tick; skipped %u non-ASCII/unsupported bytes",
            injected,
            primary ? "primary" : "clipboard",
            skipped);
  }

  return consumed;
}

static void replace_active_paste(uint8_t *text,
                                 uint32_t text_size,
                                 bool primary) {
  free(g_active_paste.text);

  g_active_paste.text = text;
  g_active_paste.size = text_size;
  g_active_paste.offset = 0;
  g_active_paste.primary = primary;
}

static void drain_active_paste_chunk(struct wd_server *server) {
  if (!g_active_paste.text) {
    return;
  }

  if (g_active_paste.offset >= g_active_paste.size) {
    free(g_active_paste.text);
    memset(&g_active_paste, 0, sizeof(g_active_paste));
    return;
  }

  uint32_t consumed = inject_text_as_keys_limited(
      server,
      g_active_paste.text + g_active_paste.offset,
      g_active_paste.size - g_active_paste.offset,
      g_active_paste.primary,
      WD_PASTE_CHARS_PER_TICK);

  if (consumed == 0) {
    free(g_active_paste.text);
    memset(&g_active_paste, 0, sizeof(g_active_paste));
    return;
  }

  g_active_paste.offset += consumed;

  if (g_active_paste.offset >= g_active_paste.size) {
    free(g_active_paste.text);
    memset(&g_active_paste, 0, sizeof(g_active_paste));
  }
}

void wd_clipboard_drain_and_apply(struct wd_server *server) {
  if (!server || !server->seat || !server->display) {
    return;
  }

  uint8_t *clipboard_text = NULL;
  uint32_t clipboard_text_size = 0;
  bool have_clipboard = false;

  uint8_t *primary_text = NULL;
  uint32_t primary_text_size = 0;
  bool have_primary = false;

  pthread_mutex_lock(&server->net.lock);

  if (server->net.clipboard_text_pending) {
    clipboard_text = server->net.clipboard_text;
    clipboard_text_size = server->net.clipboard_text_size;
    server->net.clipboard_text = NULL;
    server->net.clipboard_text_size = 0;
    server->net.clipboard_text_pending = false;
    have_clipboard = true;
  }

  if (server->net.primary_text_pending) {
    primary_text = server->net.primary_text;
    primary_text_size = server->net.primary_text_size;
    server->net.primary_text = NULL;
    server->net.primary_text_size = 0;
    server->net.primary_text_pending = false;
    have_primary = true;
  }

  pthread_mutex_unlock(&server->net.lock);

  if (have_clipboard) {
    replace_active_paste(clipboard_text, clipboard_text_size, false);
  }

  if (have_primary) {
    replace_active_paste(primary_text, primary_text_size, true);
  }

  drain_active_paste_chunk(server);
}
