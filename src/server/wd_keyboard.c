#include "wd_server.h"

#include <stdlib.h>
#include <string.h>

#include "waydisplay/wd_time.h"

bool wd_keyboard_init(struct wd_server *server) {
    server->keyboard_group = wlr_keyboard_group_create();
    if (!server->keyboard_group) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create keyboard group");
        return false;
    }

    server->keyboard = &server->keyboard_group->keyboard;

    struct xkb_context *xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_context) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create xkb context");
        return false;
    }

    struct xkb_rule_names rules = {
        .rules = getenv("XKB_DEFAULT_RULES"),
        .model = getenv("XKB_DEFAULT_MODEL"),
        .layout = getenv("XKB_DEFAULT_LAYOUT"),
        .variant = getenv("XKB_DEFAULT_VARIANT"),
        .options = getenv("XKB_DEFAULT_OPTIONS"),
    };

    if (!rules.layout) {
        rules.layout = "us";
    }

    struct xkb_keymap *keymap =
    xkb_keymap_new_from_names(xkb_context,
                              &rules,
                              XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (!keymap) {
        wlr_log(WLR_ERROR, "WayDisplay: failed to create xkb keymap");
        xkb_context_unref(xkb_context);
        return false;
    }

    wlr_keyboard_set_keymap(server->keyboard, keymap);
    wlr_keyboard_set_repeat_info(server->keyboard, 25, 600);
    wlr_seat_set_keyboard(server->seat, server->keyboard);

    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb_context);

    return true;
}

void wd_keyboard_queue_event_locked(struct wd_net_state *net,
                                    const struct wd_keyboard_event_payload *event) {
    if (net->key_queue_count >= WD_KEY_QUEUE_CAP) {
        net->stats.key_events_dropped++;
        return;
    }

    struct wd_queued_key_event *dst = &net->key_queue[net->key_queue_count++];

    dst->evdev_key_code = event->evdev_key_code;
    dst->pressed = event->pressed != 0;
    dst->client_timestamp_ns = event->client_timestamp_ns;

    net->stats.key_events_rx++;
                                    }
void wd_keyboard_drain_and_inject(struct wd_server *server) {
                                        struct wd_queued_key_event local[WD_KEY_QUEUE_CAP];
                                        size_t count = 0;

                                        pthread_mutex_lock(&server->net.lock);

                                        count = server->net.key_queue_count;

                                        if (count > WD_KEY_QUEUE_CAP) {
                                            count = WD_KEY_QUEUE_CAP;
                                        }

                                        if (count > 0) {
                                            memcpy(local, server->net.key_queue, count * sizeof(local[0]));
                                            server->net.key_queue_count = 0;
                                        }

                                        pthread_mutex_unlock(&server->net.lock);

                                        if (count == 0 || !server->seat || !server->keyboard) {
                                            return;
                                        }

                                        wlr_seat_set_keyboard(server->seat, server->keyboard);

                                        for (size_t i = 0; i < count; ++i) {
                                            enum wl_keyboard_key_state state =
                                            local[i].pressed
                                            ? WL_KEYBOARD_KEY_STATE_PRESSED
                                            : WL_KEYBOARD_KEY_STATE_RELEASED;

                                            /*
                                             * Keep old working behavior:
                                             * client sends evdev keycode and server passes it directly.
                                             */
                                            wlr_seat_keyboard_notify_key(server->seat,
                                                                         (uint32_t)(wd_now_ns() / 1000000ull),
                                                                         local[i].evdev_key_code,
                                                                         state);

                                            pthread_mutex_lock(&server->net.lock);
                                            server->net.stats.key_events_injected++;
                                            pthread_mutex_unlock(&server->net.lock);
                                        }
                                    }
