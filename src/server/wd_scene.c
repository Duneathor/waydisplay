#include "wd_server.h"

#include <stdlib.h>

static void view_configure_idle(void *data);
static void view_handle_commit(struct wl_listener *listener, void *data);
static void view_handle_map(struct wl_listener *listener, void *data);
static void view_handle_unmap(struct wl_listener *listener, void *data);
static void view_handle_destroy(struct wl_listener *listener, void *data);
static void server_handle_new_xdg_surface(struct wl_listener *listener, void *data);
static void server_handle_new_xdg_toplevel(struct wl_listener *listener, void *data);

void wd_scene_init_listeners(struct wd_server *server) {
    server->new_xdg_surface.notify = server_handle_new_xdg_surface;
    wl_signal_add(&server->xdg_shell->events.new_surface,
                  &server->new_xdg_surface);

    server->new_xdg_toplevel.notify = server_handle_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel,
                  &server->new_xdg_toplevel);
}

void wd_scene_set_view_position(struct wd_view *view) {
    if (view && view->scene_tree) {
        wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);
    }
}

void wd_scene_focus_view(struct wd_view *view) {
    if (!view ||
        !view->mapped ||
        !view->xdg_surface ||
        view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
        !view->xdg_surface->toplevel) {
        return;
        }

        wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);

    wlr_seat_set_capabilities(view->server->seat,
                              WL_SEAT_CAPABILITY_POINTER |
                              WL_SEAT_CAPABILITY_KEYBOARD);

    if (view->server->keyboard) {
        wlr_seat_set_keyboard(view->server->seat,
                              view->server->keyboard);

        wlr_seat_keyboard_notify_enter(view->server->seat,
                                       view->xdg_surface->surface,
                                       view->server->keyboard->keycodes,
                                       view->server->keyboard->num_keycodes,
                                       &view->server->keyboard->modifiers);
    }
}

static void view_configure_idle(void *data) {
    struct wd_view *view = data;

    if (!view) {
        return;
    }

    view->configure_idle = NULL;

    if (!view->xdg_surface ||
        view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
        !view->xdg_surface->toplevel ||
        view->configured_once) {
        return;
        }

        /*
         * wlroots 0.19: defer initial configure until idle.
         */
        wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
                                  WD_DISPLAY_WIDTH,
                                  WD_DISPLAY_HEIGHT);
        wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);

        view->configured_once = true;

        wlr_log(WLR_INFO,
                "WayDisplay: sent initial xdg toplevel configure");
}

static void view_handle_commit(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_view *view =
    wl_container_of(listener, view, commit);

    if (!view->configured_once && !view->configure_idle) {
        view->configure_idle =
        wl_event_loop_add_idle(view->server->event_loop,
                               view_configure_idle,
                               view);
    }
}

static void view_handle_map(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_view *view =
    wl_container_of(listener, view, map);

    view->mapped = true;

    wd_scene_set_view_position(view);
    wd_scene_focus_view(view);

    wlr_log(WLR_INFO,
            "WayDisplay: xdg toplevel mapped scene_tree=%p",
            (void *)view->scene_tree);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_view *view =
    wl_container_of(listener, view, unmap);

    view->mapped = false;
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_view *view =
    wl_container_of(listener, view, destroy);

    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->commit.link);

    if (view->configure_idle) {
        wl_event_source_remove(view->configure_idle);
        view->configure_idle = NULL;
    }

    /*
     * Do not destroy scene_tree manually.
     * wlroots owns the scene node lifetime for xdg surface scene nodes.
     */
    if (view->scene_tree) {
        view->scene_tree->node.data = NULL;
        view->scene_tree = NULL;
    }

    free(view);
}

static void server_handle_new_xdg_surface(struct wl_listener *listener,
                                          void *data) {
    (void)listener;

    struct wlr_xdg_surface *xdg_surface = data;

    wlr_log(WLR_INFO,
            "WayDisplay: new xdg surface role=%d",
            xdg_surface->role);
                                          }

                                          static void server_handle_new_xdg_toplevel(struct wl_listener *listener,
                                                                                     void *data) {
                                              struct wd_server *server =
                                              wl_container_of(listener, server, new_xdg_toplevel);

                                              struct wlr_xdg_toplevel *toplevel = data;
                                              struct wlr_xdg_surface *xdg_surface = toplevel->base;

                                              struct wd_view *view = calloc(1, sizeof(*view));
                                              if (!view) {
                                                  return;
                                              }

                                              view->server = server;
                                              view->xdg_surface = xdg_surface;

                                              view->scene_tree =
                                              wlr_scene_xdg_surface_create(&server->scene->tree,
                                                                           xdg_surface);

                                              if (view->scene_tree) {
                                                  view->scene_tree->node.data = view;
                                                  wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);
                                              }

                                              view->commit.notify = view_handle_commit;
                                              wl_signal_add(&xdg_surface->surface->events.commit,
                                                            &view->commit);

                                              view->map.notify = view_handle_map;
                                              wl_signal_add(&xdg_surface->surface->events.map,
                                                            &view->map);

                                              view->unmap.notify = view_handle_unmap;
                                              wl_signal_add(&xdg_surface->surface->events.unmap,
                                                            &view->unmap);

                                              view->destroy.notify = view_handle_destroy;
                                              wl_signal_add(&xdg_surface->events.destroy,
                                                            &view->destroy);

                                              wlr_log(WLR_INFO,
                                                      "WayDisplay: new xdg toplevel scene_tree=%p",
                                                      (void *)view->scene_tree);

                                              view->configure_idle =
                                              wl_event_loop_add_idle(server->event_loop,
                                                                     view_configure_idle,
                                                                     view);
                                                                                     }
