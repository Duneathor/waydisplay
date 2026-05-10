#include "wd_server.h"

#include <stdlib.h>

static void view_configure_idle(void *data);
static void view_handle_commit(struct wl_listener *listener, void *data);
static void view_handle_map(struct wl_listener *listener, void *data);
static void view_handle_unmap(struct wl_listener *listener, void *data);
static void view_handle_destroy(struct wl_listener *listener, void *data);
static void server_handle_new_xdg_surface(struct wl_listener *listener,
                                          void *data);
static void server_handle_new_xdg_toplevel(struct wl_listener *listener,
                                           void *data);
static void view_handle_request_move(struct wl_listener *listener, void *data);

void wd_scene_init_listeners(struct wd_server *server) {
  server->new_xdg_surface.notify = server_handle_new_xdg_surface;
  wl_signal_add(&server->xdg_shell->events.new_surface,
                &server->new_xdg_surface);

  server->new_xdg_toplevel.notify = server_handle_new_xdg_toplevel;
  wl_signal_add(&server->xdg_shell->events.new_toplevel,
                &server->new_xdg_toplevel);
}

struct wd_view *wd_scene_view_at(struct wd_server *server, double lx, double ly,
                                 double *sx, double *sy) {
  if (!server) {
    return NULL;
  }

  struct wd_view *view;

  /*
   * Tail is topmost because wd_scene_raise_view() and new-window insertion
   * should insert at server->views.prev.
   */
  wl_list_for_each_reverse(view, &server->views, link) {
    if (!view->mapped || !view->xdg_surface || !view->xdg_surface->surface) {
      continue;
    }

    int surface_w = view->xdg_surface->surface->current.width;
    int surface_h = view->xdg_surface->surface->current.height;

    if (surface_w <= 0) {
      surface_w = WD_DISPLAY_WIDTH;
    }

    if (surface_h <= 0) {
      surface_h = WD_DISPLAY_HEIGHT;
    }

    if (lx < view->x || ly < view->y || lx >= view->x + surface_w ||
        ly >= view->y + surface_h) {
      continue;
    }

    if (sx) {
      *sx = lx - view->x;
    }

    if (sy) {
      *sy = ly - view->y;
    }

    return view;
  }

  return NULL;
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

void wd_scene_set_view_position(struct wd_view *view) {
  if (!view || !view->scene_tree) {
    return;
  }

  wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
}

void wd_scene_focus_view(struct wd_view *view) {
  if (!view || !view->mapped || !view->xdg_surface ||
      view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
      !view->xdg_surface->toplevel) {
    return;
  }

  struct wd_server *server = view->server;

  server->focused_view = view;
  server->focused_surface = view->xdg_surface->surface;

  wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);

  wd_scene_raise_view(view);

  wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER |
                                              WL_SEAT_CAPABILITY_KEYBOARD);

  if (server->keyboard) {
    wlr_seat_set_keyboard(server->seat, server->keyboard);

    wlr_seat_keyboard_notify_enter(
        server->seat, server->focused_surface, server->keyboard->keycodes,
        server->keyboard->num_keycodes, &server->keyboard->modifiers);
  }

  wd_server_mark_scene_dirty(server);
}

static void view_configure_idle(void *data) {
  struct wd_view *view = data;

  if (!view) {
    return;
  }

  view->configure_idle = NULL;

  if (!view->xdg_surface ||
      view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
      !view->xdg_surface->toplevel || view->configured_once) {
    return;
  }

  /*
   * wlroots 0.19: defer initial configure until idle.
   */
  uint32_t width = WD_DISPLAY_WIDTH;
  uint32_t height = WD_DISPLAY_HEIGHT;

  if (view->x != 0 || view->y != 0) {
    width = WD_DISPLAY_WIDTH - 160;
    height = WD_DISPLAY_HEIGHT - 120;
  }

  wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, width, height);

  wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);

  view->configured_once = true;

  wlr_log(WLR_INFO, "WayDisplay: sent initial xdg toplevel configure");
}

void wd_scene_raise_view(struct wd_view *view) {
  if (!view || !view->scene_tree) {
    return;
  }

  /*
   * Raise in wlroots scene graph.
   */
  wlr_scene_node_raise_to_top(&view->scene_tree->node);

  /*
   * Also move to the tail of our view list.
   * wd_scene_view_at() walks reverse, so tail == topmost.
   */
  wl_list_remove(&view->link);
  wl_list_insert(view->server->views.prev, &view->link);

  wd_server_mark_scene_dirty(view->server);
}

static void view_handle_commit(struct wl_listener *listener, void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, commit);

  wd_server_mark_scene_dirty(view->server);

  if (!view->configured_once && !view->configure_idle) {
    view->configure_idle = wl_event_loop_add_idle(view->server->event_loop,
                                                  view_configure_idle, view);
  }
}

static void view_handle_map(struct wl_listener *listener, void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, map);

  view->mapped = true;

  wd_scene_set_view_position(view);
  wd_scene_focus_view(view);
  wd_server_mark_scene_dirty(view->server);

  wlr_log(WLR_INFO, "WayDisplay: xdg toplevel mapped scene_tree=%p",
          (void *)view->scene_tree);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, unmap);

  view->mapped = false;
  wd_server_mark_scene_dirty(view->server);
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;

    struct wd_view *view =
    wl_container_of(listener, view, destroy);

    struct wd_server *server = view->server;

    if (view->link.prev && view->link.next) {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    remove_listener_if_linked(&view->map);
    remove_listener_if_linked(&view->unmap);
    remove_listener_if_linked(&view->commit);
    remove_listener_if_linked(&view->request_move);
    remove_listener_if_linked(&view->destroy);

    if (view->configure_idle) {
        wl_event_source_remove(view->configure_idle);
        view->configure_idle = NULL;
    }

    if (server->focused_view == view) {
        server->focused_view = NULL;
        server->focused_surface = NULL;
    }

    if (server->focused_surface == view->xdg_surface->surface) {
        server->focused_surface = NULL;
    }

    if (server->move_grab.view == view) {
        server->move_grab.active = false;
        server->move_grab.view = NULL;
    }

    if (view->scene_tree) {
        view->scene_tree->node.data = NULL;
        view->scene_tree = NULL;
    }

    wd_server_mark_scene_dirty(server);

    free(view);
}

static void view_handle_request_move(struct wl_listener *listener, void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, request_move);

  if (!view || !view->mapped) {
    return;
  }

  wd_scene_focus_view(view);
  wd_pointer_begin_move(view->server, view);
}

static void server_handle_new_xdg_surface(struct wl_listener *listener,
                                          void *data) {
  (void)listener;

  struct wlr_xdg_surface *xdg_surface = data;

  if (!xdg_surface) {
    return;
  }

  wlr_log(WLR_INFO, "WayDisplay: new xdg surface role=%d", xdg_surface->role);
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

  wl_list_init(&view->link);
  wl_list_init(&view->map.link);
  wl_list_init(&view->unmap.link);
  wl_list_init(&view->destroy.link);
  wl_list_init(&view->commit.link);
  wl_list_init(&view->request_move.link);

  view->server = server;
  view->xdg_surface = xdg_surface;
  int offset = (int)((server->next_view_offset++ % 8) * 40);

  view->x = offset;
  view->y = offset;

  view->scene_tree =
      wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface);

  if (view->scene_tree) {
    view->scene_tree->node.data = view;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
  }

  wl_list_insert(server->views.prev, &view->link);

  view->commit.notify = view_handle_commit;
  wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);

  view->map.notify = view_handle_map;
  wl_signal_add(&xdg_surface->surface->events.map, &view->map);

  view->request_move.notify = view_handle_request_move;
  wl_signal_add(&xdg_surface->toplevel->events.request_move,
                &view->request_move);

  view->unmap.notify = view_handle_unmap;
  wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);

  view->destroy.notify = view_handle_destroy;
  wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

  wlr_log(WLR_INFO, "WayDisplay: new xdg toplevel scene_tree=%p",
          (void *)view->scene_tree);

  view->configure_idle =
      wl_event_loop_add_idle(server->event_loop, view_configure_idle, view);
}
