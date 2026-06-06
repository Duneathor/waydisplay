#include "wd_server.h"

#include <stdlib.h>

struct wd_xdg_decoration {
    struct wlr_xdg_toplevel_decoration_v1* decoration;

    struct wl_listener destroy;
    struct wl_listener request_mode;

    struct wl_event_source* configure_idle;
};

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

static void set_client_side_decoration(struct wd_xdg_decoration* decoration) {
    if (!decoration || !decoration->decoration)
    {
        return;
    }

    /*
     * WayDisplay does not draw compositor-side title bars/borders yet, so ask
     * clients that support xdg-decoration to keep drawing their own chrome.
     * Advertising the protocol is still important because toolkits such as Qt
     * and GTK use the configure result to settle decoration behavior instead of
     * guessing from the absence of the global.
     */
    wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static void xdg_decoration_configure_idle(void* data) {
    struct wd_xdg_decoration* decoration = data;

    if (!decoration)
    {
        return;
    }

    decoration->configure_idle = NULL;
    set_client_side_decoration(decoration);
}

static void schedule_decoration_configure(struct wd_xdg_decoration* decoration) {
    if (!decoration || !decoration->decoration || decoration->configure_idle)
    {
        return;
    }

    struct wl_display*    display    = wl_client_get_display(decoration->decoration->resource->client);
    struct wl_event_loop* event_loop = wl_display_get_event_loop(display);

    decoration->configure_idle = wl_event_loop_add_idle(event_loop, xdg_decoration_configure_idle, decoration);
}

static void handle_xdg_decoration_request_mode(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_xdg_decoration* decoration = wl_container_of(listener, decoration, request_mode);

    schedule_decoration_configure(decoration);
}

static void handle_xdg_decoration_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_xdg_decoration* decoration = wl_container_of(listener, decoration, destroy);

    remove_listener_if_linked(&decoration->request_mode);
    remove_listener_if_linked(&decoration->destroy);

    if (decoration->configure_idle)
    {
        wl_event_source_remove(decoration->configure_idle);
        decoration->configure_idle = NULL;
    }

    free(decoration);
}

static void handle_new_xdg_toplevel_decoration(struct wl_listener* listener, void* data) {
    struct wd_server*                      server         = wl_container_of(listener, server, new_xdg_toplevel_decoration);
    struct wlr_xdg_toplevel_decoration_v1* wlr_decoration = data;

    if (!wlr_decoration)
    {
        return;
    }

    struct wd_xdg_decoration* decoration = calloc(1, sizeof(*decoration));
    if (!decoration)
    {
        WD_LOG_ERROR("WayDisplay: failed to allocate xdg-decoration state");
        return;
    }

    decoration->decoration = wlr_decoration;
    wl_list_init(&decoration->destroy.link);
    wl_list_init(&decoration->request_mode.link);

    decoration->destroy.notify = handle_xdg_decoration_destroy;
    wl_signal_add(&wlr_decoration->events.destroy, &decoration->destroy);

    decoration->request_mode.notify = handle_xdg_decoration_request_mode;
    wl_signal_add(&wlr_decoration->events.request_mode, &decoration->request_mode);

    schedule_decoration_configure(decoration);

    (void)server;
    WD_LOG_DEBUG("WayDisplay: queued xdg-decoration client-side mode");
}

bool wd_xdg_decoration_init(struct wd_server* server) {
    if (!server || !server->display)
    {
        return false;
    }

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    if (!server->xdg_decoration_manager)
    {
        WD_LOG_ERROR("WayDisplay: failed to create xdg-decoration manager");
        return false;
    }

    server->new_xdg_toplevel_decoration.notify = handle_new_xdg_toplevel_decoration;
    wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration, &server->new_xdg_toplevel_decoration);

    return true;
}

void wd_xdg_decoration_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    remove_listener_if_linked(&server->new_xdg_toplevel_decoration);
    server->xdg_decoration_manager = NULL;
}
