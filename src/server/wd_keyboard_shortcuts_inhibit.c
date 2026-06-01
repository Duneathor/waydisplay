#include "wd_server.h"

#include <stdlib.h>

#include "waydisplay/wd_log.h"

struct wd_keyboard_shortcuts_inhibitor_state {
    struct wl_list link;
    struct wd_server *server;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    struct wl_listener destroy;
};

static bool inhibitor_should_be_active(
    struct wd_server *server,
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
    return server && inhibitor && inhibitor->seat == server->seat &&
           inhibitor->surface && inhibitor->surface == server->focused_surface;
}

void wd_keyboard_shortcuts_inhibit_refresh(struct wd_server *server) {
    if (!server || !server->keyboard_shortcuts_inhibit_manager) {
        return;
    }

    struct wd_keyboard_shortcuts_inhibitor_state *state = NULL;
    wl_list_for_each(state, &server->keyboard_shortcuts_inhibitors, link) {
        if (!state->inhibitor) {
            continue;
        }

        const bool should_activate =
            inhibitor_should_be_active(server, state->inhibitor);

        if (should_activate && !state->inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_activate(state->inhibitor);
            WD_LOG_INFO(
                "WayDisplay: keyboard shortcuts inhibited for focused surface=%p",
                (void *)state->inhibitor->surface);
        } else if (!should_activate && state->inhibitor->active) {
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(state->inhibitor);
            WD_LOG_INFO(
                "WayDisplay: keyboard shortcuts restored for surface=%p",
                (void *)state->inhibitor->surface);
        }
    }
}

bool wd_keyboard_shortcuts_inhibit_active(struct wd_server *server) {
    if (!server || !server->keyboard_shortcuts_inhibit_manager) {
        return false;
    }

    struct wd_keyboard_shortcuts_inhibitor_state *state = NULL;
    wl_list_for_each(state, &server->keyboard_shortcuts_inhibitors, link) {
        if (state->inhibitor && state->inhibitor->active &&
            inhibitor_should_be_active(server, state->inhibitor)) {
            return true;
        }
    }

    return false;
}

static void inhibitor_state_destroy(
    struct wd_keyboard_shortcuts_inhibitor_state *state) {
    if (!state) {
        return;
    }

    if (state->destroy.link.prev && state->destroy.link.next) {
        wl_list_remove(&state->destroy.link);
        wl_list_init(&state->destroy.link);
    }

    if (state->link.prev && state->link.next) {
        wl_list_remove(&state->link);
        wl_list_init(&state->link);
    }

    free(state);
}

static void handle_inhibitor_destroy(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_keyboard_shortcuts_inhibitor_state *state =
        wl_container_of(listener, state, destroy);
    struct wd_server *server = state->server;

    inhibitor_state_destroy(state);
    wd_keyboard_shortcuts_inhibit_refresh(server);
}

static void handle_new_keyboard_shortcuts_inhibitor(struct wl_listener *listener,
                                                    void *data) {
    struct wd_server *server =
        wl_container_of(listener, server, new_keyboard_shortcuts_inhibitor);
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;

    if (!server || !inhibitor) {
        return;
    }

    struct wd_keyboard_shortcuts_inhibitor_state *state = calloc(1, sizeof(*state));
    if (!state) {
        WD_LOG_ERROR("WayDisplay: failed to allocate keyboard-shortcuts inhibitor state");
        return;
    }

    state->server = server;
    state->inhibitor = inhibitor;
    wl_list_insert(&server->keyboard_shortcuts_inhibitors, &state->link);

    state->destroy.notify = handle_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &state->destroy);

    WD_LOG_INFO(
        "WayDisplay: keyboard-shortcuts-inhibit requested surface=%p seat=%p",
        (void *)inhibitor->surface,
        (void *)inhibitor->seat);

    wd_keyboard_shortcuts_inhibit_refresh(server);
}

static void handle_keyboard_shortcuts_inhibit_manager_destroy(
    struct wl_listener *listener,
    void *data) {
    (void)data;

    struct wd_server *server =
        wl_container_of(listener, server, keyboard_shortcuts_inhibit_manager_destroy);

    server->keyboard_shortcuts_inhibit_manager = NULL;

    if (server->new_keyboard_shortcuts_inhibitor.link.prev &&
        server->new_keyboard_shortcuts_inhibitor.link.next) {
        wl_list_remove(&server->new_keyboard_shortcuts_inhibitor.link);
        wl_list_init(&server->new_keyboard_shortcuts_inhibitor.link);
    }

    if (server->keyboard_shortcuts_inhibit_manager_destroy.link.prev &&
        server->keyboard_shortcuts_inhibit_manager_destroy.link.next) {
        wl_list_remove(&server->keyboard_shortcuts_inhibit_manager_destroy.link);
        wl_list_init(&server->keyboard_shortcuts_inhibit_manager_destroy.link);
    }
}

bool wd_keyboard_shortcuts_inhibit_init(struct wd_server *server) {
    if (!server || !server->display) {
        return false;
    }

    wl_list_init(&server->keyboard_shortcuts_inhibitors);
    wl_list_init(&server->new_keyboard_shortcuts_inhibitor.link);
    wl_list_init(&server->keyboard_shortcuts_inhibit_manager_destroy.link);

    server->keyboard_shortcuts_inhibit_manager =
        wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
    if (!server->keyboard_shortcuts_inhibit_manager) {
        WD_LOG_ERROR(
            "WayDisplay: failed to create keyboard-shortcuts-inhibit manager");
        return false;
    }

    server->new_keyboard_shortcuts_inhibitor.notify =
        handle_new_keyboard_shortcuts_inhibitor;
    wl_signal_add(
        &server->keyboard_shortcuts_inhibit_manager->events.new_inhibitor,
        &server->new_keyboard_shortcuts_inhibitor);

    server->keyboard_shortcuts_inhibit_manager_destroy.notify =
        handle_keyboard_shortcuts_inhibit_manager_destroy;
    wl_signal_add(
        &server->keyboard_shortcuts_inhibit_manager->events.destroy,
        &server->keyboard_shortcuts_inhibit_manager_destroy);

    WD_LOG_INFO("WayDisplay: keyboard-shortcuts-inhibit enabled");

    return true;
}

void wd_keyboard_shortcuts_inhibit_destroy(struct wd_server *server) {
    if (!server) {
        return;
    }

    struct wd_keyboard_shortcuts_inhibitor_state *state = NULL;
    struct wd_keyboard_shortcuts_inhibitor_state *tmp = NULL;
    wl_list_for_each_safe(state, tmp, &server->keyboard_shortcuts_inhibitors, link) {
        inhibitor_state_destroy(state);
    }

    if (server->new_keyboard_shortcuts_inhibitor.link.prev &&
        server->new_keyboard_shortcuts_inhibitor.link.next) {
        wl_list_remove(&server->new_keyboard_shortcuts_inhibitor.link);
        wl_list_init(&server->new_keyboard_shortcuts_inhibitor.link);
    }

    if (server->keyboard_shortcuts_inhibit_manager_destroy.link.prev &&
        server->keyboard_shortcuts_inhibit_manager_destroy.link.next) {
        wl_list_remove(&server->keyboard_shortcuts_inhibit_manager_destroy.link);
        wl_list_init(&server->keyboard_shortcuts_inhibit_manager_destroy.link);
    }

    server->keyboard_shortcuts_inhibit_manager = NULL;
}
