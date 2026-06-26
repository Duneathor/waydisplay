#include "waydisplay/wd_input.h"
#include "waydisplay/wd_time.h"
#include "wd_server_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <xkbcommon/xkbcommon.h>

bool wd_keyboard_init(struct wd_server* server) {
    if (!server || !server->seat)
    {
        return false;
    }

    server->keyboard_group = wlr_keyboard_group_create();
    if (!server->keyboard_group)
    {
        WD_LOG_ERROR("failed to create keyboard group");
        return false;
    }

    server->keyboard = &server->keyboard_group->keyboard;

    struct xkb_context* xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_context)
    {
        WD_LOG_ERROR("failed to create xkb context");
        wlr_keyboard_group_destroy(server->keyboard_group);
        server->keyboard_group = NULL;
        server->keyboard       = NULL;
        return false;
    }

    struct xkb_rule_names rules = {
        .rules   = getenv("XKB_DEFAULT_RULES"),
        .model   = getenv("XKB_DEFAULT_MODEL"),
        .layout  = getenv("XKB_DEFAULT_LAYOUT"),
        .variant = getenv("XKB_DEFAULT_VARIANT"),
        .options = getenv("XKB_DEFAULT_OPTIONS"),
    };

    if (!rules.layout)
    {
        rules.layout = "us";
    }

    struct xkb_keymap* keymap = xkb_keymap_new_from_names(xkb_context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (!keymap)
    {
        WD_LOG_ERROR("failed to create xkb keymap");
        xkb_context_unref(xkb_context);
        wlr_keyboard_group_destroy(server->keyboard_group);
        server->keyboard_group = NULL;
        server->keyboard       = NULL;
        return false;
    }

    wlr_keyboard_set_keymap(server->keyboard, keymap);
    wlr_keyboard_set_repeat_info(server->keyboard, WD_SERVER_KEYBOARD_REPEAT_RATE_HZ, WD_SERVER_KEYBOARD_REPEAT_DELAY_MS);
    wlr_seat_set_keyboard(server->seat, server->keyboard);

    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb_context);

    return true;
}

/*
 * Client timestamps are monotonic-clock values from another process and often
 * another host.  They are useful as opaque ordering/debug payload, but they
 * cannot be subtracted from the server monotonic clock to produce latency.
 */
static void wd_stats_note_input_inject_locked(struct wd_net_state* net, uint64_t server_rx_timestamp_ns, uint64_t inject_timestamp_ns) {
    if (!net || server_rx_timestamp_ns == 0 || inject_timestamp_ns == 0 || inject_timestamp_ns < server_rx_timestamp_ns)
    {
        return;
    }

    net->stats.input_queue_latency_samples++;
    net->stats.input_queue_latency_sum_ns += inject_timestamp_ns - server_rx_timestamp_ns;
    net->last_input_inject_ns        = inject_timestamp_ns;
    net->input_since_last_summary    = true;
    net->input_since_last_fresh_tile = true;
}

void wd_keyboard_queue_event_locked(struct wd_net_state* net, const struct wd_keyboard_event_payload* event,
                                    uint64_t server_rx_timestamp_ns) {
    if (net->key_queue_count >= WD_SERVER_KEY_QUEUE_CAPACITY)
    {
        net->stats.key_events_dropped++;
        return;
    }

    struct wd_queued_key_event* dst = &net->key_queue[net->key_queue_count++];

    dst->evdev_key_code         = event->evdev_key_code;
    dst->pressed                = event->pressed != 0;
    dst->client_timestamp_ns    = event->client_timestamp_ns;
    dst->input_sequence         = event->input_sequence;
    dst->server_rx_timestamp_ns = server_rx_timestamp_ns;

    net->stats.key_events_rx++;
}

static uint32_t key_time_msec(const struct wd_queued_key_event* event) {
    if (event && event->server_rx_timestamp_ns != 0)
    {
        return (uint32_t)(event->server_rx_timestamp_ns / WD_NSEC_PER_MSEC);
    }

    return (uint32_t)(wd_now_ns() / WD_NSEC_PER_MSEC);
}

static ssize_t wd_keyboard_find_pressed_key(const struct wd_server* server, uint32_t evdev_key_code) {
    if (!server)
    {
        return -1;
    }

    for (size_t i = 0; i < server->pressed_keycode_count; ++i)
    {
        if (server->pressed_keycodes[i] == evdev_key_code)
        {
            return (ssize_t)i;
        }
    }

    return -1;
}

void wd_keyboard_note_key_state(struct wd_server* server, uint32_t evdev_key_code, bool pressed) {
    if (!server)
    {
        return;
    }

    const ssize_t index = wd_keyboard_find_pressed_key(server, evdev_key_code);

    if (pressed)
    {
        if (index >= 0)
        {
            server->net.stats.key_state_duplicate_presses++;
            return;
        }

        if (server->pressed_keycode_count >= WD_SERVER_PRESSED_KEY_CAPACITY)
        {
            server->net.stats.key_events_dropped++;
            return;
        }

        server->pressed_keycodes[server->pressed_keycode_count++] = evdev_key_code;
        return;
    }

    if (index < 0)
    {
        server->net.stats.key_state_release_without_press++;
        return;
    }

    const size_t remove_index = (size_t)index;
    if (remove_index + 1 < server->pressed_keycode_count)
    {
        memmove(&server->pressed_keycodes[remove_index], &server->pressed_keycodes[remove_index + 1],
                (server->pressed_keycode_count - remove_index - 1) * sizeof(server->pressed_keycodes[0]));
    }
    server->pressed_keycode_count--;
}

void wd_keyboard_clear_pressed_keys(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    server->pressed_keycode_count = 0;
}

void wd_keyboard_notify_enter(struct wd_server* server, struct wlr_surface* surface) {
    if (!server || !server->seat || !server->keyboard || !surface)
    {
        return;
    }

    wlr_seat_set_keyboard(server->seat, server->keyboard);
    wlr_seat_keyboard_notify_enter(server->seat, surface, server->pressed_keycodes, server->pressed_keycode_count,
                                   &server->keyboard->modifiers);
    server->net.stats.keyboard_enter_events++;
}

static void notify_key_and_modifiers(struct wd_server* server, const struct wd_queued_key_event* event) {
    if (!server || !server->seat || !server->keyboard || !event)
    {
        return;
    }

    enum wl_keyboard_key_state state = event->pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;

    const uint32_t time_msec = key_time_msec(event);

    /*
     * The SDL client sends Linux evdev keycodes. wlroots 0.19 does not expose
     * wlr_keyboard_notify_key(), so update the keyboard's xkb state directly
     * before forwarding the key through the seat. xkb keycodes are evdev + 8.
     */
    if (server->keyboard->xkb_state)
    {
        enum xkb_key_direction direction = event->pressed ? XKB_KEY_DOWN : XKB_KEY_UP;

        xkb_state_update_key(server->keyboard->xkb_state, event->evdev_key_code + WD_INPUT_XKB_KEYCODE_OFFSET, direction);

        server->keyboard->modifiers.depressed = xkb_state_serialize_mods(server->keyboard->xkb_state, XKB_STATE_MODS_DEPRESSED);
        server->keyboard->modifiers.latched   = xkb_state_serialize_mods(server->keyboard->xkb_state, XKB_STATE_MODS_LATCHED);
        server->keyboard->modifiers.locked    = xkb_state_serialize_mods(server->keyboard->xkb_state, XKB_STATE_MODS_LOCKED);
        server->keyboard->modifiers.group     = xkb_state_serialize_layout(server->keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

        if (server->focused_surface)
        {
            wlr_seat_keyboard_notify_modifiers(server->seat, &server->keyboard->modifiers);
        }
    }

    wd_keyboard_note_key_state(server, event->evdev_key_code, event->pressed);

    wlr_seat_keyboard_notify_key(server->seat, time_msec, event->evdev_key_code, state);
}

void wd_keyboard_drain_and_inject(struct wd_server* server) {
    struct wd_queued_key_event local[WD_SERVER_KEY_QUEUE_CAPACITY];
    size_t                     count = 0;

    pthread_mutex_lock(&server->net.lock);

    const bool reset_key_state = server->net.key_state_reset_pending;
    if (reset_key_state)
    {
        server->net.key_state_reset_pending = false;
    }

    count = server->net.key_queue_count;
    if (count > WD_SERVER_KEY_QUEUE_CAPACITY)
    {
        count = WD_SERVER_KEY_QUEUE_CAPACITY;
    }

    if (count > 0)
    {
        memcpy(local, server->net.key_queue, count * sizeof(local[0]));
        server->net.key_queue_count = 0;
    }

    pthread_mutex_unlock(&server->net.lock);

    if (reset_key_state)
    {
        wd_keyboard_clear_pressed_keys(server);
    }

    if (count == 0 || !server->seat || !server->keyboard)
    {
        return;
    }

    /*
     * Keyboard enter is a focus-transition event. Re-sending it before every
     * input batch can create duplicate enter/leave traffic and can expose stale
     * held-key state if focus changes while keys are down. wd_scene_focus_view()
     * owns keyboard-enter delivery; normal batches only update modifiers and
     * keys for the already-focused surface.
     */
    wlr_seat_set_keyboard(server->seat, server->keyboard);

    for (size_t i = 0; i < count; ++i)
    {
        notify_key_and_modifiers(server, &local[i]);

        pthread_mutex_lock(&server->net.lock);
        server->net.last_input_sequence = local[i].input_sequence;
        wd_stats_note_input_inject_locked(&server->net, local[i].server_rx_timestamp_ns, wd_now_ns());
        server->net.stats.key_events_injected++;
        pthread_mutex_unlock(&server->net.lock);
    }
}
