#include "wd_server.h"

#if WAYDISPLAY_ENABLE_XWAYLAND

#include <stdlib.h>
#include <string.h>

#define WD_XWAYLAND_DEFAULT_WIDTH 800u
#define WD_XWAYLAND_DEFAULT_HEIGHT 600u
#define WD_XWAYLAND_MIN_VISIBLE_WIDTH 64u
#define WD_XWAYLAND_MIN_VISIBLE_HEIGHT 48u

static bool listener_is_linked(struct wl_listener *listener) {
    return listener &&
           listener->link.prev &&
           listener->link.next &&
           listener->link.prev != &listener->link &&
           listener->link.next != &listener->link;
}

static void remove_listener_if_linked(struct wl_listener *listener) {
    if (!listener) {
        return;
    }

    if (listener_is_linked(listener)) {
        wl_list_remove(&listener->link);
    }

    wl_list_init(&listener->link);
}

static char *dup_or_empty(const char *text) {
    if (!text) {
        text = "";
    }

    char *copy = strdup(text);
    return copy ? copy : strdup("");
}

static uint16_t sane_width(uint16_t width) {
    return width >= WD_XWAYLAND_MIN_VISIBLE_WIDTH ?
        width : WD_XWAYLAND_DEFAULT_WIDTH;
}

static uint16_t sane_height(uint16_t height) {
    return height >= WD_XWAYLAND_MIN_VISIBLE_HEIGHT ?
        height : WD_XWAYLAND_DEFAULT_HEIGHT;
}

static void xwayland_view_update_metadata(struct wd_view *view) {
    if (!view || !view->xwayland_surface) {
        return;
    }

    char *title = dup_or_empty(view->xwayland_surface->title);
    char *app_id = dup_or_empty(view->xwayland_surface->class);

    if (title) {
        free(view->title);
        view->title = title;
    }

    if (app_id) {
        free(view->app_id);
        view->app_id = app_id;
    }
}

static void xwayland_view_clear_focus_and_grabs(struct wd_view *view) {
    if (!view || !view->server) {
        return;
    }

    struct wd_server *server = view->server;

    if (server->focused_view == view) {
        server->focused_view = NULL;
        server->focused_surface = NULL;
        wd_keyboard_shortcuts_inhibit_refresh(server);

        if (server->seat) {
            wlr_seat_pointer_notify_clear_focus(server->seat);
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }

    if (server->move_grab.view == view) {
        server->move_grab.active = false;
        server->move_grab.view = NULL;
    }

    if (server->resize_grab.view == view) {
        server->resize_grab.active = false;
        server->resize_grab.view = NULL;
    }
}

static void xwayland_view_configure_current_geometry(struct wd_view *view) {
    if (!view || !view->xwayland_surface) {
        return;
    }

    struct wlr_xwayland_surface *xsurface = view->xwayland_surface;
    uint16_t width = sane_width(xsurface->width);
    uint16_t height = sane_height(xsurface->height);

    wlr_xwayland_surface_configure(xsurface,
                                   view->x,
                                   view->y,
                                   width,
                                   height);
}

static void xwayland_view_mark_mapped(struct wd_view *view, bool focus) {
    if (!view || !view->xwayland_surface || !view->server) {
        return;
    }

    xwayland_view_update_metadata(view);

    view->x = view->xwayland_surface->x;
    view->y = view->xwayland_surface->y;
    view->mapped = true;
    view->minimized = false;

    wd_scene_set_view_position(view);

    if (focus && view->scene_tree) {
        wd_scene_focus_view(view);
    }

    wd_server_mark_scene_dirty(view->server);
}

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_server *server =
        wl_container_of(listener, server, xwayland_ready);

    if (!server || !server->xwayland) {
        return;
    }

    if (server->seat) {
        wlr_xwayland_set_seat(server->xwayland, server->seat);
    }

    if (server->xwayland->display_name) {
        setenv("DISPLAY", server->xwayland->display_name, 1);
        WD_LOG_INFO("WayDisplay: Xwayland ready on DISPLAY=%s",
                    server->xwayland->display_name);
    } else {
        WD_LOG_INFO("WayDisplay: Xwayland ready");
    }
}

static void handle_xwayland_surface_commit(struct wl_listener *listener,
                                           void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_commit);

    if (view && view->server) {
        wd_server_mark_scene_dirty(view->server);
    }
}

static void handle_xwayland_surface_map(struct wl_listener *listener,
                                        void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_map);
    if (!view || !view->xwayland_surface || !view->server) {
        return;
    }

    xwayland_view_mark_mapped(view, true);

    WD_LOG_DEBUG(
        "WayDisplay: Xwayland surface mapped view=%p geom=%dx%d+%d+%d title=%s class=%s",
        (void *)view,
        (int)view->xwayland_surface->width,
        (int)view->xwayland_surface->height,
        (int)view->xwayland_surface->x,
        (int)view->xwayland_surface->y,
        view->title ? view->title : "",
        view->app_id ? view->app_id : "");
}

static void handle_xwayland_surface_unmap(struct wl_listener *listener,
                                          void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_unmap);
    if (!view) {
        return;
    }

    view->mapped = false;
    view->activated = false;
    xwayland_view_clear_focus_and_grabs(view);

    if (view->server) {
        wd_server_mark_scene_dirty(view->server);
    }
}

static void xwayland_view_disassociate(struct wd_view *view) {
    if (!view) {
        return;
    }

    remove_listener_if_linked(&view->xwayland_map);
    remove_listener_if_linked(&view->xwayland_unmap);
    remove_listener_if_linked(&view->xwayland_commit);

    if (view->scene_tree) {
        view->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }

    view->mapped = false;
    view->activated = false;
    xwayland_view_clear_focus_and_grabs(view);

    if (view->server) {
        wd_server_mark_scene_dirty(view->server);
    }
}

static void xwayland_view_attach_surface_listeners(struct wd_view *view) {
    if (!view || !view->xwayland_surface || !view->xwayland_surface->surface) {
        return;
    }

    struct wlr_surface *surface = view->xwayland_surface->surface;

    if (!listener_is_linked(&view->xwayland_map)) {
        view->xwayland_map.notify = handle_xwayland_surface_map;
        wl_signal_add(&surface->events.map, &view->xwayland_map);
    }

    if (!listener_is_linked(&view->xwayland_unmap)) {
        view->xwayland_unmap.notify = handle_xwayland_surface_unmap;
        wl_signal_add(&surface->events.unmap, &view->xwayland_unmap);
    }

    if (!listener_is_linked(&view->xwayland_commit)) {
        view->xwayland_commit.notify = handle_xwayland_surface_commit;
        wl_signal_add(&surface->events.commit, &view->xwayland_commit);
    }
}

static void xwayland_view_associate(struct wd_view *view) {
    if (!view || !view->server || !view->server->scene ||
        !view->xwayland_surface || !view->xwayland_surface->surface) {
        return;
    }

    if (!view->scene_tree) {
        view->scene_tree =
            wlr_scene_subsurface_tree_create(&view->server->scene->tree,
                                             view->xwayland_surface->surface);
        if (view->scene_tree) {
            view->scene_tree->node.data = view;
            wd_scene_set_view_position(view);
        }
    }

    xwayland_view_attach_surface_listeners(view);

    /*
     * Depending on wlroots/Xwayland event ordering, the wlr_surface map event
     * can already have fired by the time the Xwayland associate event gives us
     * a usable wlr_surface pointer.  In that case our map listener will never
     * run, so promote the associated surface into WayDisplay's mapped/focused
     * view state here as well.
     */
    if (view->mapped || view->xwayland_surface->surface->mapped) {
        xwayland_view_mark_mapped(view, true);
    } else {
        wd_server_mark_scene_dirty(view->server);
    }

    WD_LOG_DEBUG(
        "WayDisplay: Xwayland associated view=%p scene_tree=%p mapped=%d",
        (void *)view,
        (void *)view->scene_tree,
        view->mapped ? 1 : 0);
}

static void handle_xwayland_associate(struct wl_listener *listener,
                                      void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_associate);
    xwayland_view_associate(view);
}

static void handle_xwayland_dissociate(struct wl_listener *listener,
                                       void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_dissociate);
    xwayland_view_disassociate(view);
}

static void handle_xwayland_map_request(struct wl_listener *listener,
                                        void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_map_request);
    if (!view || !view->xwayland_surface) {
        return;
    }

    xwayland_view_mark_mapped(view, view->scene_tree != NULL);
    xwayland_view_configure_current_geometry(view);

    WD_LOG_DEBUG(
        "WayDisplay: Xwayland map request view=%p requested=%dx%d+%d+%d configured=%ux%u pending_associate=%d",
        (void *)view,
        (int)view->xwayland_surface->width,
        (int)view->xwayland_surface->height,
        (int)view->xwayland_surface->x,
        (int)view->xwayland_surface->y,
        (unsigned)sane_width(view->xwayland_surface->width),
        (unsigned)sane_height(view->xwayland_surface->height),
        view->scene_tree ? 0 : 1);
}

static void handle_xwayland_request_configure(struct wl_listener *listener,
                                              void *data) {
    struct wd_view *view =
        wl_container_of(listener, view, xwayland_request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;

    if (!view || !view->xwayland_surface || !event) {
        return;
    }

    view->x = event->x;
    view->y = event->y;

    uint16_t width = sane_width(event->width);
    uint16_t height = sane_height(event->height);

    wlr_xwayland_surface_configure(view->xwayland_surface,
                                   event->x,
                                   event->y,
                                   width,
                                   height);
    wd_scene_set_view_position(view);

    if (view->server) {
        wd_server_mark_scene_dirty(view->server);
    }
}

static void handle_xwayland_surface_destroy(struct wl_listener *listener,
                                            void *data) {
    (void)data;

    struct wd_view *view = wl_container_of(listener, view, xwayland_destroy);
    if (!view) {
        return;
    }

    struct wd_server *server = view->server;

    xwayland_view_clear_focus_and_grabs(view);

    remove_listener_if_linked(&view->xwayland_destroy);
    remove_listener_if_linked(&view->xwayland_associate);
    remove_listener_if_linked(&view->xwayland_dissociate);
    remove_listener_if_linked(&view->xwayland_map);
    remove_listener_if_linked(&view->xwayland_unmap);
    remove_listener_if_linked(&view->xwayland_commit);
    remove_listener_if_linked(&view->xwayland_map_request);
    remove_listener_if_linked(&view->xwayland_request_configure);

    if (view->link.prev && view->link.next) {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    if (view->scene_tree) {
        view->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }

    free(view->app_id);
    free(view->title);
    free(view);

    if (server) {
        wd_server_mark_scene_dirty(server);
    }
}

static void handle_new_xwayland_surface(struct wl_listener *listener,
                                        void *data) {
    struct wd_server *server =
        wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;

    if (!server || !xsurface || !server->scene) {
        return;
    }

    struct wd_view *view = calloc(1, sizeof(*view));
    if (!view) {
        return;
    }

    wl_list_init(&view->link);
    wl_list_init(&view->xwayland_destroy.link);
    wl_list_init(&view->xwayland_associate.link);
    wl_list_init(&view->xwayland_dissociate.link);
    wl_list_init(&view->xwayland_map.link);
    wl_list_init(&view->xwayland_unmap.link);
    wl_list_init(&view->xwayland_commit.link);
    wl_list_init(&view->xwayland_map_request.link);
    wl_list_init(&view->xwayland_request_configure.link);

    view->server = server;
    view->xwayland_surface = xsurface;
    view->x = xsurface->x;
    view->y = xsurface->y;
    view->positioned = true;

    xwayland_view_update_metadata(view);

    wl_list_insert(server->views.prev, &view->link);

    view->xwayland_destroy.notify = handle_xwayland_surface_destroy;
    wl_signal_add(&xsurface->events.destroy, &view->xwayland_destroy);

    view->xwayland_associate.notify = handle_xwayland_associate;
    wl_signal_add(&xsurface->events.associate, &view->xwayland_associate);

    view->xwayland_dissociate.notify = handle_xwayland_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &view->xwayland_dissociate);

    view->xwayland_map_request.notify = handle_xwayland_map_request;
    wl_signal_add(&xsurface->events.map_request, &view->xwayland_map_request);

    view->xwayland_request_configure.notify = handle_xwayland_request_configure;
    wl_signal_add(&xsurface->events.request_configure,
                  &view->xwayland_request_configure);

    if (xsurface->surface) {
        xwayland_view_associate(view);
    }

    WD_LOG_DEBUG("WayDisplay: new Xwayland shell surface view=%p",
                 (void *)view);
}

bool wd_xwayland_init(struct wd_server *server) {
    if (!server || !server->display || !server->compositor) {
        return false;
    }

    server->xwayland = wlr_xwayland_create(server->display,
                                           server->compositor,
                                           true);
    if (!server->xwayland) {
        WD_LOG_ERROR("WayDisplay: failed to create Xwayland");
        return false;
    }

    wl_list_init(&server->xwayland_ready.link);
    wl_list_init(&server->new_xwayland_surface.link);

    server->xwayland_ready.notify = handle_xwayland_ready;
    wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);

    server->new_xwayland_surface.notify = handle_new_xwayland_surface;
    wl_signal_add(&server->xwayland->events.new_surface,
                  &server->new_xwayland_surface);

    if (server->xwayland->display_name) {
        WD_LOG_INFO("WayDisplay: Xwayland enabled on DISPLAY=%s",
                    server->xwayland->display_name);
    } else {
        WD_LOG_INFO("WayDisplay: Xwayland enabled");
    }

    return true;
}

void wd_xwayland_destroy(struct wd_server *server) {
    if (!server) {
        return;
    }

    remove_listener_if_linked(&server->xwayland_ready);
    remove_listener_if_linked(&server->new_xwayland_surface);

    if (server->xwayland) {
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }
}

#endif
