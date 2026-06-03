#include "waydisplay/wd_time.h"
#include "wd_server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <xkbcommon/xkbcommon.h>

#define WD_PASTE_MAX_TEXT_BYTES (64u * 1024u)

struct wd_remote_data_source {
    struct wlr_data_source source;
    struct wd_server*      server;
    uint8_t*               text;
    uint32_t               text_size;
};

struct wd_remote_primary_source {
    struct wlr_primary_selection_source source;
    struct wd_server*                   server;
    uint8_t*                            text;
    uint32_t                            text_size;
};

static bool is_text_mime_type(const char* mime_type) {
    return mime_type && (strcmp(mime_type, "text/plain;charset=utf-8") == 0 || strcmp(mime_type, "text/plain") == 0 ||
                         strcmp(mime_type, "UTF8_STRING") == 0 || strcmp(mime_type, "TEXT") == 0 || strcmp(mime_type, "STRING") == 0);
}

static bool add_mime_type(struct wl_array* mime_types, const char* mime_type) {
    if (!mime_types || !mime_type)
    {
        return false;
    }

    char* copy = strdup(mime_type);
    if (!copy)
    {
        return false;
    }

    char** slot = wl_array_add(mime_types, sizeof(*slot));
    if (!slot)
    {
        free(copy);
        return false;
    }

    *slot = copy;
    return true;
}

static bool add_text_mime_types(struct wl_array* mime_types) {
    return add_mime_type(mime_types, "text/plain;charset=utf-8") && add_mime_type(mime_types, "text/plain") &&
           add_mime_type(mime_types, "UTF8_STRING") && add_mime_type(mime_types, "TEXT") && add_mime_type(mime_types, "STRING");
}

static void write_all_to_fd(int fd, const uint8_t* data, uint32_t size) {
    uint32_t offset = 0;

    while (offset < size)
    {
        ssize_t written = write(fd, data + offset, (size_t)(size - offset));
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (written == 0)
        {
            break;
        }

        offset += (uint32_t)written;
    }

    close(fd);
}

static void remote_data_source_send(struct wlr_data_source* source, const char* mime_type, int fd) {
    struct wd_remote_data_source* remote = wl_container_of(source, remote, source);
    struct wd_server*             server = remote->server;

    if (!server || !is_text_mime_type(mime_type))
    {
        WD_LOG_DEBUG("WayDisplay: rejecting remote clipboard request for mime=%s", mime_type ? mime_type : "(null)");
        close(fd);
        return;
    }

    WD_LOG_DEBUG("WayDisplay: sending remote clipboard selection mime=%s size=%u", mime_type, remote->text_size);

    write_all_to_fd(fd, remote->text, remote->text_size);
}

static void remote_data_source_destroy(struct wlr_data_source* source) {
    struct wd_remote_data_source* remote = wl_container_of(source, remote, source);

    if (remote->server && remote->server->remote_clipboard_source == source)
    {
        remote->server->remote_clipboard_source = NULL;
    }

    free(remote->text);
    free(remote);
}

static const struct wlr_data_source_impl remote_data_source_impl = {
    .send    = remote_data_source_send,
    .destroy = remote_data_source_destroy,
};

static void remote_primary_source_send(struct wlr_primary_selection_source* source, const char* mime_type, int fd) {
    struct wd_remote_primary_source* remote = wl_container_of(source, remote, source);
    struct wd_server*                server = remote->server;

    if (!server || !is_text_mime_type(mime_type))
    {
        WD_LOG_DEBUG("WayDisplay: rejecting remote primary request for mime=%s", mime_type ? mime_type : "(null)");
        close(fd);
        return;
    }

    WD_LOG_DEBUG("WayDisplay: sending remote primary selection mime=%s size=%u", mime_type, remote->text_size);

    write_all_to_fd(fd, remote->text, remote->text_size);
}

static void remote_primary_source_destroy(struct wlr_primary_selection_source* source) {
    struct wd_remote_primary_source* remote = wl_container_of(source, remote, source);

    if (remote->server && remote->server->remote_primary_source == source)
    {
        remote->server->remote_primary_source = NULL;
    }

    free(remote->text);
    free(remote);
}

static const struct wlr_primary_selection_source_impl remote_primary_source_impl = {
    .send    = remote_primary_source_send,
    .destroy = remote_primary_source_destroy,
};

static void clear_remote_selection_source(struct wd_server* server, bool primary);

static void handle_request_set_selection(struct wl_listener* listener, void* data) {
    struct wd_server*                            server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event* event  = data;

    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener* listener, void* data) {
    struct wd_server*                                    server = wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event* event  = data;

    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void remove_listener_if_linked(struct wl_listener* listener) {
    if (!listener)
    {
        return;
    }

    if (listener->link.prev && listener->link.next)
    {
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }
}

bool wd_clipboard_init(struct wd_server* server) {
    if (!server || !server->display || !server->seat)
    {
        return false;
    }

    server->data_device_manager = wlr_data_device_manager_create(server->display);

    if (!server->data_device_manager)
    {
        WD_LOG_ERROR("WayDisplay: failed to create data device manager");
        return false;
    }

    server->primary_selection_manager = wlr_primary_selection_v1_device_manager_create(server->display);

    if (!server->primary_selection_manager)
    {
        WD_LOG_ERROR("WayDisplay: failed to create primary selection manager");
        return false;
    }

    server->request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);

    server->request_set_primary_selection.notify = handle_request_set_primary_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection, &server->request_set_primary_selection);

    return true;
}

void wd_clipboard_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    remove_listener_if_linked(&server->request_set_selection);
    remove_listener_if_linked(&server->request_set_primary_selection);

    clear_remote_selection_source(server, false);
    clear_remote_selection_source(server, true);

    free(server->remote_clipboard_text);
    server->remote_clipboard_text      = NULL;
    server->remote_clipboard_text_size = 0;

    free(server->remote_primary_text);
    server->remote_primary_text      = NULL;
    server->remote_primary_text_size = 0;
}

static bool payload_to_text_copy(uint32_t expected_session_id, const uint8_t* payload, uint32_t payload_size, uint8_t** out_text,
                                 uint32_t* out_text_size) {
    if (out_text)
    {
        *out_text = NULL;
    }

    if (out_text_size)
    {
        *out_text_size = 0;
    }

    if (!payload || !out_text || !out_text_size || payload_size < sizeof(struct wd_selection_payload_header))
    {
        return false;
    }

    struct wd_selection_payload_header header;
    memcpy(&header, payload, sizeof(header));

    if (header.session_id != expected_session_id ||
        (header.mime_type != WD_SELECTION_MIME_TEXT_UTF8 && header.mime_type != WD_SELECTION_MIME_TEXT_PLAIN) ||
        header.data_size > WD_PASTE_MAX_TEXT_BYTES)
    {
        return false;
    }

    size_t needed = sizeof(header) + (size_t)header.data_size;
    if (payload_size < needed)
    {
        return false;
    }

    uint8_t* text = calloc((size_t)header.data_size + 1u, 1);
    if (!text)
    {
        return false;
    }

    if (header.data_size > 0)
    {
        memcpy(text, payload + sizeof(header), header.data_size);
    }

    *out_text      = text;
    *out_text_size = header.data_size;
    return true;
}

void wd_clipboard_queue_client_set_locked(struct wd_net_state* net, uint32_t expected_session_id, const uint8_t* payload,
                                          uint32_t payload_size, bool primary) {
    if (!net)
    {
        return;
    }

    uint8_t* text      = NULL;
    uint32_t text_size = 0;

    if (!payload_to_text_copy(expected_session_id, payload, payload_size, &text, &text_size))
    {
        return;
    }

    if (primary)
    {
        free(net->primary_text);
        net->primary_text         = text;
        net->primary_text_size    = text_size;
        net->primary_text_pending = true;
    }
    else
    {
        free(net->clipboard_text);
        net->clipboard_text          = text;
        net->clipboard_text_size     = text_size;
        net->clipboard_text_pending  = true;
        net->clipboard_paste_pending = true;
    }
}

static void clear_remote_selection_source(struct wd_server* server, bool primary) {
    if (!server || !server->seat || !server->display)
    {
        return;
    }

    uint32_t serial = wl_display_next_serial(server->display);

    if (primary)
    {
        if (server->remote_primary_source)
        {
            wlr_seat_set_primary_selection(server->seat, NULL, serial);
            server->remote_primary_source = NULL;
        }
    }
    else
    {
        if (server->remote_clipboard_source)
        {
            wlr_seat_set_selection(server->seat, NULL, serial);
            server->remote_clipboard_source = NULL;
        }
    }
}

static bool set_source_text_copy(uint8_t** dst_text, uint32_t* dst_size, const uint8_t* src_text, uint32_t src_size) {
    if (!dst_text || !dst_size || (!src_text && src_size > 0))
    {
        return false;
    }

    uint8_t* copy = calloc((size_t)src_size + 1u, 1);
    if (!copy)
    {
        return false;
    }

    if (src_size > 0)
    {
        memcpy(copy, src_text, src_size);
    }

    *dst_text = copy;
    *dst_size = src_size;
    return true;
}

static bool publish_remote_clipboard_selection(struct wd_server* server) {
    if (!server || !server->seat || !server->display)
    {
        return false;
    }

    struct wd_remote_data_source* source = calloc(1, sizeof(*source));
    if (!source)
    {
        return false;
    }

    source->server = server;

    if (!set_source_text_copy(&source->text, &source->text_size, server->remote_clipboard_text, server->remote_clipboard_text_size))
    {
        free(source);
        return false;
    }

    wlr_data_source_init(&source->source, &remote_data_source_impl);

    if (!add_text_mime_types(&source->source.mime_types))
    {
        wlr_data_source_destroy(&source->source);
        return false;
    }

    uint32_t serial = wl_display_next_serial(server->display);
    wlr_seat_set_selection(server->seat, &source->source, serial);
    server->remote_clipboard_source = &source->source;
    return true;
}

static bool publish_remote_primary_selection(struct wd_server* server) {
    if (!server || !server->seat || !server->display)
    {
        return false;
    }

    struct wd_remote_primary_source* source = calloc(1, sizeof(*source));
    if (!source)
    {
        return false;
    }

    source->server = server;

    if (!set_source_text_copy(&source->text, &source->text_size, server->remote_primary_text, server->remote_primary_text_size))
    {
        free(source);
        return false;
    }

    wlr_primary_selection_source_init(&source->source, &remote_primary_source_impl);

    if (!add_text_mime_types(&source->source.mime_types))
    {
        wlr_primary_selection_source_destroy(&source->source);
        return false;
    }

    uint32_t serial = wl_display_next_serial(server->display);
    wlr_seat_set_primary_selection(server->seat, &source->source, serial);
    server->remote_primary_source = &source->source;
    return true;
}

static void update_keyboard_modifiers_for_key(struct wd_server* server, uint32_t evdev_key_code, bool pressed) {
    if (!server || !server->keyboard || !server->keyboard->xkb_state)
    {
        return;
    }

    enum xkb_key_direction direction = pressed ? XKB_KEY_DOWN : XKB_KEY_UP;
    xkb_state_update_key(server->keyboard->xkb_state, evdev_key_code + 8, direction);

    server->keyboard->modifiers.depressed = xkb_state_serialize_mods(server->keyboard->xkb_state, XKB_STATE_MODS_DEPRESSED);
    server->keyboard->modifiers.latched   = xkb_state_serialize_mods(server->keyboard->xkb_state, XKB_STATE_MODS_LATCHED);
    server->keyboard->modifiers.locked    = xkb_state_serialize_mods(server->keyboard->xkb_state, XKB_STATE_MODS_LOCKED);
    server->keyboard->modifiers.group     = xkb_state_serialize_layout(server->keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

    if (server->seat && server->focused_surface)
    {
        wlr_seat_keyboard_notify_modifiers(server->seat, &server->keyboard->modifiers);
    }
}

static void synthesize_key(struct wd_server* server, uint32_t evdev_key_code, bool pressed, uint32_t time_msec) {
    update_keyboard_modifiers_for_key(server, evdev_key_code, pressed);

    enum wl_keyboard_key_state state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;

    wlr_seat_keyboard_notify_key(server->seat, time_msec, evdev_key_code, state);
}

static bool keyboard_modifier_active(struct wd_server* server, const char* name) {
    if (!server || !server->keyboard || !server->keyboard->xkb_state || !name)
    {
        return false;
    }

    return xkb_state_mod_name_is_active(server->keyboard->xkb_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void synthesize_clipboard_paste_shortcut(struct wd_server* server) {
    if (!server || !server->seat || !server->keyboard || !server->focused_surface)
    {
        return;
    }

    /* Linux evdev key codes: KEY_LEFTCTRL and KEY_V. */
    const uint32_t key_leftctrl         = 29;
    const uint32_t key_v                = 47;
    const uint32_t time_msec            = (uint32_t)(wd_now_ns() / 1000000ull);
    const bool     ctrl_already_active  = keyboard_modifier_active(server, XKB_MOD_NAME_CTRL);
    const bool     shift_already_active = keyboard_modifier_active(server, XKB_MOD_NAME_SHIFT);

    wlr_seat_set_keyboard(server->seat, server->keyboard);
    wlr_seat_keyboard_notify_enter(server->seat, server->focused_surface, server->keyboard->keycodes, server->keyboard->num_keycodes,
                                   &server->keyboard->modifiers);

    if (shift_already_active)
    {
        /*
         * Preserve Ctrl+Shift+V for terminals and other clients whose normal paste
         * accelerator is Ctrl+Shift+V.  Ctrl/Shift have already been forwarded by
         * the keyboard drain before clipboard drain runs, so only replay V while
         * those modifiers are held.
         */
        WD_LOG_DEBUG("WayDisplay: synthesizing V with existing Ctrl+Shift paste modifiers");
        synthesize_key(server, key_v, true, time_msec);
        synthesize_key(server, key_v, false, time_msec);
        return;
    }

    if (ctrl_already_active)
    {
        /*
         * Prefer the real, already-forwarded Ctrl modifier when available. This
         * avoids disturbing the user's modifier state while still replaying the
         * suppressed V key after publishing the data-device selection.
         */
        WD_LOG_DEBUG("WayDisplay: synthesizing V with existing Ctrl paste modifier");
        synthesize_key(server, key_v, true, time_msec);
        synthesize_key(server, key_v, false, time_msec);
        return;
    }

    /* Fallback for unusual event ordering where Ctrl was not still depressed. */
    WD_LOG_DEBUG("WayDisplay: synthesizing full Ctrl+V paste trigger");
    synthesize_key(server, key_leftctrl, true, time_msec);
    synthesize_key(server, key_v, true, time_msec);
    synthesize_key(server, key_v, false, time_msec);
    synthesize_key(server, key_leftctrl, false, time_msec);
}

static bool remote_selection_matches(struct wlr_data_source* source, const uint8_t* text, uint32_t text_size) {
    if (!source)
    {
        return false;
    }

    struct wd_remote_data_source* remote = wl_container_of(source, remote, source);
    return remote->text_size == text_size && (text_size == 0 || memcmp(remote->text, text, text_size) == 0);
}

static void store_remote_selection(struct wd_server* server, uint8_t* text, uint32_t text_size, bool primary) {
    if (!server)
    {
        free(text);
        return;
    }

    if (text_size == 0)
    {
        free(text);

        if (primary)
        {
            free(server->remote_primary_text);
            server->remote_primary_text      = NULL;
            server->remote_primary_text_size = 0;
        }
        else
        {
            free(server->remote_clipboard_text);
            server->remote_clipboard_text      = NULL;
            server->remote_clipboard_text_size = 0;
        }

        clear_remote_selection_source(server, primary);
        WD_LOG_DEBUG("WayDisplay: cleared remote %s selection", primary ? "primary" : "clipboard");
        return;
    }

    bool published = false;

    if (primary)
    {
        free(server->remote_primary_text);
        server->remote_primary_text      = text;
        server->remote_primary_text_size = text_size;
        published                        = publish_remote_primary_selection(server);
    }
    else
    {
        if (remote_selection_matches(server->remote_clipboard_source, text, text_size))
        {
            free(server->remote_clipboard_text);
            server->remote_clipboard_text      = text;
            server->remote_clipboard_text_size = text_size;
            published                          = true;
        }
        else
        {
            free(server->remote_clipboard_text);
            server->remote_clipboard_text      = text;
            server->remote_clipboard_text_size = text_size;
            published                          = publish_remote_clipboard_selection(server);
        }
    }

    if (!published)
    {
        WD_LOG_ERROR("WayDisplay: failed to publish remote %s selection", primary ? "primary" : "clipboard");
        return;
    }

    WD_LOG_DEBUG("WayDisplay: published remote %s selection (%u bytes)", primary ? "primary" : "clipboard", text_size);
}

void wd_clipboard_drain_and_apply(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    uint8_t* clipboard_text      = NULL;
    uint32_t clipboard_text_size = 0;
    bool     have_clipboard      = false;
    bool     paste_requested     = false;

    uint8_t* primary_text      = NULL;
    uint32_t primary_text_size = 0;
    bool     have_primary      = false;

    pthread_mutex_lock(&server->net.lock);

    if (server->net.clipboard_text_pending)
    {
        clipboard_text                      = server->net.clipboard_text;
        clipboard_text_size                 = server->net.clipboard_text_size;
        server->net.clipboard_text          = NULL;
        server->net.clipboard_text_size     = 0;
        server->net.clipboard_text_pending  = false;
        paste_requested                     = server->net.clipboard_paste_pending;
        server->net.clipboard_paste_pending = false;
        have_clipboard                      = true;
    }

    if (server->net.primary_text_pending)
    {
        primary_text                     = server->net.primary_text;
        primary_text_size                = server->net.primary_text_size;
        server->net.primary_text         = NULL;
        server->net.primary_text_size    = 0;
        server->net.primary_text_pending = false;
        have_primary                     = true;
    }

    pthread_mutex_unlock(&server->net.lock);

    if (have_clipboard)
    {
        store_remote_selection(server, clipboard_text, clipboard_text_size, false);
        if (paste_requested)
        {
            synthesize_clipboard_paste_shortcut(server);
        }
    }

    if (have_primary)
    {
        store_remote_selection(server, primary_text, primary_text_size, true);
    }
}
