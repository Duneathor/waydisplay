#include "wd_server.h"

#include <stdlib.h>

static void view_configure_idle(void *data);
static void view_handle_commit(struct wl_listener *listener, void *data);
static void view_handle_map(struct wl_listener *listener, void *data);
static void view_handle_unmap(struct wl_listener *listener, void *data);
static void view_apply_fractional_scale(struct wd_view *view);
static void view_handle_xdg_surface_destroy(struct wl_listener *listener,
                                            void *data);
static void view_handle_xdg_toplevel_destroy(struct wl_listener *listener,
                                             void *data);
static void server_handle_new_xdg_surface(struct wl_listener *listener,
                                          void *data);
static void server_handle_new_xdg_toplevel(struct wl_listener *listener,
                                           void *data);
static void view_handle_request_move(struct wl_listener *listener, void *data);
static void view_handle_request_resize(struct wl_listener *listener,
                                       void *data);
static void view_handle_request_maximize(struct wl_listener *listener,
                                         void *data);
static void view_handle_request_fullscreen(struct wl_listener *listener,
                                           void *data);
static void view_handle_request_minimize(struct wl_listener *listener,
                                         void *data);
static void view_handle_new_popup(struct wl_listener *listener, void *data);

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

static int32_t preferred_buffer_scale_for(double scale) {
  if (scale <= 1.0) {
    return 1;
  }

  return (int32_t)(scale + 0.999999);
}

static void surface_apply_fractional_scale(struct wlr_surface *surface,
                                           int sx,
                                           int sy,
                                           void *data) {
  (void)sx;
  (void)sy;

  struct wd_server *server = data;
  if (!surface || !server) {
    return;
  }

  double scale = server->output_scale;
  if (scale <= 0.0) {
    scale = 1.0;
  }

  /*
   * wp_fractional_scale_v1 tells clients the preferred fractional scale.
   * wl_surface preferred buffer scale remains integer, so use ceil(scale)
   * there to keep clients from allocating too-small buffers.
   */
  wlr_fractional_scale_v1_notify_scale(surface, scale);
  wlr_surface_set_preferred_buffer_scale(surface,
                                         preferred_buffer_scale_for(scale));
}

static void view_apply_fractional_scale(struct wd_view *view) {
  if (!view || !view->server || !view->xdg_surface) {
    return;
  }

  wlr_xdg_surface_for_each_surface(view->xdg_surface,
                                   surface_apply_fractional_scale,
                                   view->server);
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

  double scale = view->server->output_scale;
  if (scale <= 0.0) {
    scale = 1.0;
  }

  width = (uint32_t)((double)width / scale);
  height = (uint32_t)((double)height / scale);

  if (view->x != 0 || view->y != 0) {
    width = width > 160 ? width - 160 : width;
    height = height > 120 ? height - 120 : height;
  }

  wlr_xdg_toplevel_set_bounds(view->xdg_surface->toplevel, width, height);
  wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, width, height);

  wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);

  wlr_xdg_toplevel_set_wm_capabilities(
      view->xdg_surface->toplevel,
      WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
          WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
          WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);

  view_apply_fractional_scale(view);

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
  view_apply_fractional_scale(view);

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
  view_apply_fractional_scale(view);
  wd_scene_focus_view(view);
  wd_server_mark_scene_dirty(view->server);

  wlr_log(WLR_INFO, "WayDisplay: xdg toplevel mapped scene_tree=%p",
          (void *)view->scene_tree);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, unmap);

  struct wd_server *server = view->server;

  view->mapped = false;

  if (server->focused_view == view) {
    server->focused_view = NULL;
    server->focused_surface = NULL;

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

  if (server->close_grab.view == view) {
    server->close_grab.active = false;
    server->close_grab.view = NULL;
  }

  wd_server_mark_scene_dirty(server);
}

static void view_handle_xdg_toplevel_destroy(struct wl_listener *listener,
                                             void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, xdg_toplevel_destroy);

  /*
   * request_move belongs to wlr_xdg_toplevel and must be gone before
   * wlroots destroys/checks the toplevel listener lists.
   *
   * Do NOT free view here. Surface map/unmap/commit/destroy listeners may
   * still exist and may still fire.
   */
  remove_listener_if_linked(&view->request_move);
  remove_listener_if_linked(&view->request_resize);
  remove_listener_if_linked(&view->request_maximize);
  remove_listener_if_linked(&view->request_fullscreen);
  remove_listener_if_linked(&view->request_minimize);
  remove_listener_if_linked(&view->xdg_toplevel_destroy);
}

static void view_handle_xdg_surface_destroy(struct wl_listener *listener,
                                            void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, xdg_surface_destroy);

  struct wd_server *server = view->server;

  remove_listener_if_linked(&view->request_move);
  remove_listener_if_linked(&view->request_resize);
  remove_listener_if_linked(&view->request_maximize);
  remove_listener_if_linked(&view->request_fullscreen);
  remove_listener_if_linked(&view->request_minimize);
  remove_listener_if_linked(&view->xdg_toplevel_destroy);

  remove_listener_if_linked(&view->new_popup);
  remove_listener_if_linked(&view->map);
  remove_listener_if_linked(&view->unmap);
  remove_listener_if_linked(&view->commit);
  remove_listener_if_linked(&view->xdg_surface_destroy);

  if (view->link.prev && view->link.next) {
    wl_list_remove(&view->link);
    wl_list_init(&view->link);
  }

  if (view->configure_idle) {
    wl_event_source_remove(view->configure_idle);
    view->configure_idle = NULL;
  }

  if (server->focused_view == view) {
    server->focused_view = NULL;
    server->focused_surface = NULL;

    if (server->seat) {
      wlr_seat_pointer_notify_clear_focus(server->seat);
      wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
  }

  if (view->xdg_surface &&
      server->focused_surface == view->xdg_surface->surface) {
    server->focused_surface = NULL;

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

  if (server->close_grab.view == view) {
    server->close_grab.active = false;
    server->close_grab.view = NULL;
  }

  if (view->toplevel_icon) {
    wlr_xdg_toplevel_icon_v1_unref(view->toplevel_icon);
    view->toplevel_icon = NULL;
  }

  if (view->scene_tree) {
    view->scene_tree->node.data = NULL;
    view->scene_tree = NULL;
  }

  wd_server_mark_scene_dirty(server);

  free(view);
}

static void view_handle_new_popup(struct wl_listener *listener, void *data) {
    struct wd_view *view =
    wl_container_of(listener, view, new_popup);

    struct wlr_xdg_popup *popup = data;

    if (!view || !popup) {
        return;
    }

    /*
     * wlr_scene_xdg_surface_create() should already create scene nodes for
     * the xdg surface tree. We do not create a wd_view for popups.
     *
     * The important part is that we acknowledge/configure popup creation and
     * mark the scene dirty so it gets rendered.
     */
    wlr_log(WLR_INFO,
            "WayDisplay: new xdg popup for view=%p",
            (void *)view);

    view_apply_fractional_scale(view);
    wd_server_mark_scene_dirty(view->server);
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

static void view_handle_request_resize(struct wl_listener *listener,
                                       void *data) {
  struct wd_view *view = wl_container_of(listener, view, request_resize);
  struct wlr_xdg_toplevel_resize_event *event = data;

  if (!view || !view->mapped || !event ||
      event->edges == WLR_EDGE_NONE) {
    return;
  }

  wd_scene_focus_view(view);
  wd_pointer_begin_resize(view->server, view, event->edges);
}

static void view_restore_saved_geometry(struct wd_view *view) {
  if (!view || !view->xdg_surface || !view->xdg_surface->toplevel) {
    return;
  }

  view->x = view->saved_x;
  view->y = view->saved_y;

  wd_scene_set_view_position(view);
  wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
                            view->saved_width,
                            view->saved_height);
}

static void view_save_geometry(struct wd_view *view) {
  if (!view || !view->xdg_surface || !view->xdg_surface->surface) {
    return;
  }

  view->saved_x = view->x;
  view->saved_y = view->y;

  int width = view->xdg_surface->surface->current.width;
  int height = view->xdg_surface->surface->current.height;

  if (width <= 0) {
    width = WD_DISPLAY_WIDTH;
  }

  if (height <= 0) {
    height = WD_DISPLAY_HEIGHT;
  }

  view->saved_width = (uint32_t)width;
  view->saved_height = (uint32_t)height;
}

static void view_handle_request_maximize(struct wl_listener *listener,
                                         void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, request_maximize);

  if (!view || !view->xdg_surface || !view->xdg_surface->toplevel) {
    return;
  }

  bool maximize = view->xdg_surface->toplevel->requested.maximized;

  if (maximize && !view->maximized) {
    view_save_geometry(view);

    view->x = 0;
    view->y = 0;
    wd_scene_set_view_position(view);

    double scale = view->server->output_scale;
    if (scale <= 0.0) {
      scale = 1.0;
    }

    wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
                              (uint32_t)((double)WD_DISPLAY_WIDTH / scale),
                              (uint32_t)((double)WD_DISPLAY_HEIGHT / scale));
  } else if (!maximize && view->maximized) {
    view_restore_saved_geometry(view);
  }

  view->maximized = maximize;
  wlr_xdg_toplevel_set_maximized(view->xdg_surface->toplevel, maximize);
  wd_scene_focus_view(view);
  wd_server_mark_scene_dirty(view->server);
}

static void view_handle_request_fullscreen(struct wl_listener *listener,
                                           void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, request_fullscreen);

  if (!view || !view->xdg_surface || !view->xdg_surface->toplevel) {
    return;
  }

  bool fullscreen = view->xdg_surface->toplevel->requested.fullscreen;

  if (fullscreen && !view->fullscreen) {
    view_save_geometry(view);
    view->x = 0;
    view->y = 0;
    wd_scene_set_view_position(view);

    double scale = view->server->output_scale;
    if (scale <= 0.0) {
      scale = 1.0;
    }

    wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
                              (uint32_t)((double)WD_DISPLAY_WIDTH / scale),
                              (uint32_t)((double)WD_DISPLAY_HEIGHT / scale));
  } else if (!fullscreen && view->fullscreen) {
    view_restore_saved_geometry(view);
  }

  view->fullscreen = fullscreen;
  wlr_xdg_toplevel_set_fullscreen(view->xdg_surface->toplevel, fullscreen);
  wd_scene_focus_view(view);
  wd_server_mark_scene_dirty(view->server);
}

static void view_handle_request_minimize(struct wl_listener *listener,
                                         void *data) {
  (void)data;

  struct wd_view *view = wl_container_of(listener, view, request_minimize);

  if (!view || !view->xdg_surface || !view->xdg_surface->toplevel) {
    return;
  }

  /*
   * WayDisplay does not have a taskbar/window-list yet, so there is nowhere
   * useful to park a minimized window. Acknowledge the request by scheduling a
   * configure and deactivating it, but keep it mapped.
   */
  wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, false);
  wlr_xdg_surface_schedule_configure(view->xdg_surface);
  wd_server_mark_scene_dirty(view->server);
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
  wl_list_init(&view->commit.link);
  wl_list_init(&view->request_move.link);
  wl_list_init(&view->request_resize.link);
  wl_list_init(&view->request_maximize.link);
  wl_list_init(&view->request_fullscreen.link);
  wl_list_init(&view->request_minimize.link);
  wl_list_init(&view->xdg_surface_destroy.link);
  wl_list_init(&view->xdg_toplevel_destroy.link);
  wl_list_init(&view->new_popup.link);

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

  view->unmap.notify = view_handle_unmap;
  wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);

  /*
   * Surface destroy owns final view cleanup/free.
   */
  view->xdg_surface_destroy.notify = view_handle_xdg_surface_destroy;
  wl_signal_add(&xdg_surface->events.destroy,
                &view->xdg_surface_destroy);

  /*
   * Toplevel destroy only removes toplevel-owned listeners.
   */
  view->xdg_toplevel_destroy.notify = view_handle_xdg_toplevel_destroy;
  wl_signal_add(&toplevel->events.destroy,
                &view->xdg_toplevel_destroy);

  view->request_move.notify = view_handle_request_move;
  wl_signal_add(&xdg_surface->toplevel->events.request_move,
                &view->request_move);

  view->request_resize.notify = view_handle_request_resize;
  wl_signal_add(&xdg_surface->toplevel->events.request_resize,
                &view->request_resize);

  view->request_maximize.notify = view_handle_request_maximize;
  wl_signal_add(&xdg_surface->toplevel->events.request_maximize,
                &view->request_maximize);

  view->request_fullscreen.notify = view_handle_request_fullscreen;
  wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen,
                &view->request_fullscreen);

  view->request_minimize.notify = view_handle_request_minimize;
  wl_signal_add(&xdg_surface->toplevel->events.request_minimize,
                &view->request_minimize);

  view->new_popup.notify = view_handle_new_popup;
  wl_signal_add(&xdg_surface->events.new_popup,
                &view->new_popup);

  wlr_log(WLR_INFO, "WayDisplay: new xdg toplevel scene_tree=%p",
          (void *)view->scene_tree);

  view->configure_idle =
      wl_event_loop_add_idle(server->event_loop, view_configure_idle, view);
}
