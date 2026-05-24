#include "wd_server.h"

#include <string.h>

#include <wayland-server-protocol.h>

#include "waydisplay/wd_time.h"

#include <wlr/types/wlr_scene.h>

#define WD_FALLBACK_MOVE_ZONE_HEIGHT 32.0
#define WD_RESIZE_EDGE_ZONE 8.0
#define WD_FALLBACK_CLOSE_ZONE_WIDTH 46.0
#define WD_FALLBACK_CLOSE_ZONE_HEIGHT 32.0
#define WD_MIN_WINDOW_WIDTH 120u
#define WD_MIN_WINDOW_HEIGHT 80u
#define WD_POINTER_MOD_ALT (1u << 0)
#define WD_BTN_LEFT 0x110

static struct wd_view *view_from_scene_node(struct wlr_scene_node *node) {
  while (node) {
    if (node->data) {
      return node->data;
    }

    if (!node->parent) {
      break;
    }

    node = &node->parent->node;
  }

  return NULL;
}

static bool scene_surface_at(struct wd_server *server,
                             double lx,
                             double ly,
                             struct wd_view **out_view,
                             struct wlr_surface **out_surface,
                             double *out_sx,
                             double *out_sy) {
  if (out_view) {
    *out_view = NULL;
  }

  if (out_surface) {
    *out_surface = NULL;
  }

  if (out_sx) {
    *out_sx = 0.0;
  }

  if (out_sy) {
    *out_sy = 0.0;
  }

  if (!server || !server->scene) {
    return false;
  }

  double sx = 0.0;
  double sy = 0.0;

  struct wlr_scene_node *node =
  wlr_scene_node_at(&server->scene->tree.node, lx, ly, &sx, &sy);

  if (!node) {
    return false;
  }

  struct wlr_scene_surface *scene_surface = NULL;

  if (node->type == WLR_SCENE_NODE_BUFFER) {
    struct wlr_scene_buffer *scene_buffer =
    wlr_scene_buffer_from_node(node);

    scene_surface =
    wlr_scene_surface_try_from_buffer(scene_buffer);
  }

  if (!scene_surface || !scene_surface->surface) {
    return false;
  }

  struct wd_view *view = view_from_scene_node(node);

  if (!view) {
    return false;
  }

  if (out_view) {
    *out_view = view;
  }

  if (out_surface) {
    *out_surface = scene_surface->surface;
  }

  if (out_sx) {
    *out_sx = sx;
  }

  if (out_sy) {
    *out_sy = sy;
  }

  return true;
                             }

void wd_pointer_queue_event_locked(
    struct wd_net_state *net, const struct wd_pointer_event_payload *event) {
  if (!net || !event) {
    return;
  }

  if (net->pointer_queue_count >= WD_POINTER_QUEUE_CAP) {
    memmove(&net->pointer_queue[0], &net->pointer_queue[1],
            (WD_POINTER_QUEUE_CAP - 1) * sizeof(net->pointer_queue[0]));

    net->pointer_queue_count = WD_POINTER_QUEUE_CAP - 1;
  }

  net->pointer_queue[net->pointer_queue_count++].event = *event;
}

static uint32_t view_width(struct wd_view *view) {
  if (!view || !view->xdg_surface || !view->xdg_surface->surface) {
    return WD_DISPLAY_WIDTH;
  }

  int width = view->xdg_surface->surface->current.width;
  if (width <= 0) {
    return WD_DISPLAY_WIDTH;
  }

  return (uint32_t)width;
}

static uint32_t view_height(struct wd_view *view) {
  if (!view || !view->xdg_surface || !view->xdg_surface->surface) {
    return WD_DISPLAY_HEIGHT;
  }

  int height = view->xdg_surface->surface->current.height;
  if (height <= 0) {
    return WD_DISPLAY_HEIGHT;
  }

  return (uint32_t)height;
}

static uint32_t resize_edges_at_view_point(struct wd_view *view,
                                           double sx,
                                           double sy) {
  const double width = (double)view_width(view);
  const double height = (double)view_height(view);
  uint32_t edges = WLR_EDGE_NONE;

  if (sx <= WD_RESIZE_EDGE_ZONE) edges |= WLR_EDGE_LEFT;
  if (sy <= WD_RESIZE_EDGE_ZONE) edges |= WLR_EDGE_TOP;
  if (sx >= width - WD_RESIZE_EDGE_ZONE) edges |= WLR_EDGE_RIGHT;
  if (sy >= height - WD_RESIZE_EDGE_ZONE) edges |= WLR_EDGE_BOTTOM;

  return edges;
}

static bool pointer_event_is_left_press(
    const struct wd_pointer_event_payload *event) {
  return event &&
         event->event_type == WD_POINTER_EVENT_BUTTON &&
         event->button_state == WD_POINTER_BUTTON_PRESSED &&
         event->button == WD_BTN_LEFT;
}

static bool pointer_event_is_alt_left_press(
    const struct wd_pointer_event_payload *event) {
  return pointer_event_is_left_press(event) &&
         (event->modifiers & WD_POINTER_MOD_ALT);
}

static bool view_point_is_titlebar_move_zone(double sx, double sy) {
  /*
   * Do not compositor-resize from the fallback titlebar strip. CSD clients need
   * the press so they can emit xdg_toplevel.request_move, and WayDisplay also
   * historically treated this region as the fallback move zone.
   */
  return sx >= WD_RESIZE_EDGE_ZONE && sy <= WD_FALLBACK_MOVE_ZONE_HEIGHT;
}

static bool view_point_is_close_zone(struct wd_view *view,
                                     double sx,
                                     double sy) {
  if (!view || !view->xdg_surface || !view->xdg_surface->toplevel) {
    return false;
  }

  const double width = (double)view_width(view);

  return sy >= 0.0 &&
         sy <= WD_FALLBACK_CLOSE_ZONE_HEIGHT &&
         sx >= width - WD_FALLBACK_CLOSE_ZONE_WIDTH &&
         sx <= width;
}

void wd_pointer_begin_move(struct wd_server *server, struct wd_view *view) {
  if (!server || !view) {
    return;
  }

  server->move_grab.active = true;
  server->move_grab.view = view;
  server->move_grab.grab_x = server->pointer_x;
  server->move_grab.grab_y = server->pointer_y;
  server->move_grab.view_x = view->x;
  server->move_grab.view_y = view->y;

  wlr_log(WLR_INFO,
          "WayDisplay: begin move view=%p at pointer %.1f %.1f view=%d %d",
          (void *)view, server->pointer_x, server->pointer_y, view->x, view->y);
}

void wd_pointer_update_move(struct wd_server *server) {
  if (!server || !server->move_grab.active || !server->move_grab.view) {
    return;
  }

  struct wd_view *view = server->move_grab.view;

  const double dx = server->pointer_x - server->move_grab.grab_x;
  const double dy = server->pointer_y - server->move_grab.grab_y;

  view->x = server->move_grab.view_x + (int)dx;
  view->y = server->move_grab.view_y + (int)dy;

  wd_scene_set_view_position(view);
  wd_server_mark_scene_dirty(server);
}

void wd_pointer_end_move(struct wd_server *server) {
  if (!server || !server->move_grab.active) {
    return;
  }

  wlr_log(WLR_INFO, "WayDisplay: end move");

  server->move_grab.active = false;
  server->move_grab.view = NULL;

  wd_server_mark_scene_dirty(server);
}

void wd_pointer_begin_resize(struct wd_server *server,
                             struct wd_view *view,
                             uint32_t edges) {
  if (!server || !view || !view->xdg_surface ||
      !view->xdg_surface->toplevel || edges == WLR_EDGE_NONE) {
    return;
  }

  server->resize_grab.active = true;
  server->resize_grab.view = view;
  server->resize_grab.edges = edges;
  server->resize_grab.grab_x = server->pointer_x;
  server->resize_grab.grab_y = server->pointer_y;
  server->resize_grab.view_x = view->x;
  server->resize_grab.view_y = view->y;
  server->resize_grab.view_width = view_width(view);
  server->resize_grab.view_height = view_height(view);

  wlr_xdg_toplevel_set_resizing(view->xdg_surface->toplevel, true);

  wlr_log(WLR_INFO,
          "WayDisplay: begin resize view=%p edges=0x%x pointer %.1f %.1f "
          "view=%d %d size=%ux%u",
          (void *)view,
          edges,
          server->pointer_x,
          server->pointer_y,
          view->x,
          view->y,
          server->resize_grab.view_width,
          server->resize_grab.view_height);
}

void wd_pointer_update_resize(struct wd_server *server) {
  if (!server || !server->resize_grab.active || !server->resize_grab.view) {
    return;
  }

  struct wd_view *view = server->resize_grab.view;

  if (!view->xdg_surface || !view->xdg_surface->toplevel) {
    return;
  }

  const double dx = server->pointer_x - server->resize_grab.grab_x;
  const double dy = server->pointer_y - server->resize_grab.grab_y;
  const uint32_t edges = server->resize_grab.edges;

  int new_x = server->resize_grab.view_x;
  int new_y = server->resize_grab.view_y;
  int new_width = (int)server->resize_grab.view_width;
  int new_height = (int)server->resize_grab.view_height;

  if (edges & WLR_EDGE_LEFT) {
    new_width = (int)server->resize_grab.view_width - (int)dx;
    new_x = server->resize_grab.view_x + (int)dx;

    if (new_width < (int)WD_MIN_WINDOW_WIDTH) {
      new_x -= (int)WD_MIN_WINDOW_WIDTH - new_width;
      new_width = (int)WD_MIN_WINDOW_WIDTH;
    }
  }

  if (edges & WLR_EDGE_RIGHT) {
    new_width = (int)server->resize_grab.view_width + (int)dx;

    if (new_width < (int)WD_MIN_WINDOW_WIDTH) {
      new_width = (int)WD_MIN_WINDOW_WIDTH;
    }
  }

  if (edges & WLR_EDGE_TOP) {
    new_height = (int)server->resize_grab.view_height - (int)dy;
    new_y = server->resize_grab.view_y + (int)dy;

    if (new_height < (int)WD_MIN_WINDOW_HEIGHT) {
      new_y -= (int)WD_MIN_WINDOW_HEIGHT - new_height;
      new_height = (int)WD_MIN_WINDOW_HEIGHT;
    }
  }

  if (edges & WLR_EDGE_BOTTOM) {
    new_height = (int)server->resize_grab.view_height + (int)dy;

    if (new_height < (int)WD_MIN_WINDOW_HEIGHT) {
      new_height = (int)WD_MIN_WINDOW_HEIGHT;
    }
  }

  view->x = new_x;
  view->y = new_y;

  wd_scene_set_view_position(view);
  wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel,
                            (uint32_t)new_width,
                            (uint32_t)new_height);

  wd_server_mark_scene_dirty(server);
}

void wd_pointer_end_resize(struct wd_server *server) {
  if (!server || !server->resize_grab.active) {
    return;
  }

  if (server->resize_grab.view &&
      server->resize_grab.view->xdg_surface &&
      server->resize_grab.view->xdg_surface->toplevel) {
    wlr_xdg_toplevel_set_resizing(
        server->resize_grab.view->xdg_surface->toplevel, false);
  }

  wlr_log(WLR_INFO, "WayDisplay: end resize");

  server->resize_grab.active = false;
  server->resize_grab.view = NULL;
  server->resize_grab.edges = WLR_EDGE_NONE;

  wd_server_mark_scene_dirty(server);
}

static double clamp_layout_x(uint16_t x) {
  if (x >= WD_DISPLAY_WIDTH) {
    return (double)(WD_DISPLAY_WIDTH - 1);
  }

  return (double)x;
}

static double clamp_layout_y(uint16_t y) {
  if (y >= WD_DISPLAY_HEIGHT) {
    return (double)(WD_DISPLAY_HEIGHT - 1);
  }

  return (double)y;
}

void wd_pointer_drain_and_inject(struct wd_server *server) {
  struct wd_queued_pointer_event local[WD_POINTER_QUEUE_CAP];
  size_t count = 0;

  if (!server || !server->seat) {
    return;
  }

  pthread_mutex_lock(&server->net.lock);

  count = server->net.pointer_queue_count;

  if (count > WD_POINTER_QUEUE_CAP) {
    count = WD_POINTER_QUEUE_CAP;
  }

  if (count > 0) {
    memcpy(local, server->net.pointer_queue, count * sizeof(local[0]));

    server->net.pointer_queue_count = 0;
  }

  pthread_mutex_unlock(&server->net.lock);

  if (count == 0) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const struct wd_pointer_event_payload *event = &local[i].event;

    const uint32_t time_msec =
        event->client_timestamp_ns
            ? (uint32_t)(event->client_timestamp_ns / 1000000ull)
            : (uint32_t)(wd_now_ns() / 1000000ull);

    const double lx = clamp_layout_x(event->x);
    const double ly = clamp_layout_y(event->y);

    server->pointer_x = lx;
    server->pointer_y = ly;

    /*
     * The compositor fallback close button consumes the full click pair so the
     * client does not receive a dangling release. Send close on release rather
     * than press so the surface is not destroyed while the pointer button is
     * still logically down.
     */
    if (server->close_grab.active) {
      if (event->event_type == WD_POINTER_EVENT_BUTTON &&
          event->button == WD_BTN_LEFT &&
          event->button_state == WD_POINTER_BUTTON_RELEASED) {
        struct wd_view *close_view = server->close_grab.view;
        bool should_close = false;

        if (close_view &&
            close_view->mapped &&
            close_view->xdg_surface &&
            close_view->xdg_surface->toplevel) {
          const double close_sx = server->pointer_x - close_view->x;
          const double close_sy = server->pointer_y - close_view->y;

          should_close = view_point_is_close_zone(close_view,
                                                  close_sx,
                                                  close_sy);
        }

        server->close_grab.active = false;
        server->close_grab.view = NULL;

        if (should_close) {
          wlr_log(WLR_INFO,
                  "WayDisplay: close requested for view=%p",
                  (void *)close_view);

          wlr_xdg_toplevel_send_close(close_view->xdg_surface->toplevel);
          wd_server_mark_scene_dirty(server);
        }
      }

      if (event->event_type == WD_POINTER_EVENT_BUTTON &&
          event->button == WD_BTN_LEFT &&
          event->button_state == WD_POINTER_BUTTON_RELEASED) {
        continue;
      }

      continue;
    }

    /*
     * If the compositor is currently moving a window, pointer motion updates
     * the scene position and is not forwarded to the client surface.
     */
    if (server->move_grab.active) {
      if (event->event_type == WD_POINTER_EVENT_MOTION) {
        wd_pointer_update_move(server);
        continue;
      }

      if (event->event_type == WD_POINTER_EVENT_BUTTON &&
          event->button_state == WD_POINTER_BUTTON_RELEASED) {
        wd_pointer_update_move(server);
        wd_pointer_end_move(server);
        continue;
      }

      continue;
    }

    /*
     * If the compositor is resizing a window, pointer motion updates the
     * requested toplevel size and is not forwarded to the client surface.
     */
    if (server->resize_grab.active) {
      if (event->event_type == WD_POINTER_EVENT_MOTION) {
        wd_pointer_update_resize(server);
        continue;
      }

      if (event->event_type == WD_POINTER_EVENT_BUTTON &&
          event->button_state == WD_POINTER_BUTTON_RELEASED &&
          event->button == WD_BTN_LEFT) {
        wd_pointer_update_resize(server);
        wd_pointer_end_resize(server);
        continue;
      }

      continue;
    }

    double sx = 0.0;
    double sy = 0.0;

    struct wd_view *target_view = NULL;
    struct wlr_surface *target_surface = NULL;

    if (!scene_surface_at(server,
      lx,
      ly,
      &target_view,
      &target_surface,
      &sx,
      &sy)) {
      continue;
      }

    switch (event->event_type) {
      case WD_POINTER_EVENT_MOTION:
        wlr_seat_pointer_notify_enter(server->seat,
                                      target_surface,
                                      sx,
                                      sy);

        wlr_seat_pointer_notify_motion(server->seat,
                                       time_msec,
                                       sx,
                                       sy);
        break;

      case WD_POINTER_EVENT_BUTTON:
        if (event->button_state == WD_POINTER_BUTTON_PRESSED) {
          wd_scene_focus_view(target_view);

          /*
           * Highest priority compositor gesture:
           * Alt + left mouse drag always moves the whole window, even if the
           * pointer is near an edge.
           */
          if (pointer_event_is_alt_left_press(event)) {
            wd_pointer_begin_move(server, target_view);
            break;
          }

          /*
           * Compositor fallback close:
           * Arm on press, close on release. Destroying a toplevel while the
           * pointer button is still down can leave stale focus/grab state in
           * the compositor or in strict clients.
           */
          if (pointer_event_is_left_press(event) &&
              view_point_is_close_zone(target_view, sx, sy)) {
            server->close_grab.active = true;
            server->close_grab.view = target_view;
            break;
          }

          /*
           * Compositor fallback resize:
           * Drag edges/corners of the client surface, but do not steal the
           * top titlebar/move zone from CSD clients. If the client wants a
           * move from there, it will emit xdg_toplevel.request_move and
           * wd_scene.c will call wd_pointer_begin_move().
           */
          if (pointer_event_is_left_press(event) &&
              !view_point_is_titlebar_move_zone(sx, sy)) {
            uint32_t edges = resize_edges_at_view_point(target_view, sx, sy);
            if (edges != WLR_EDGE_NONE) {
              wd_pointer_begin_resize(server, target_view, edges);
              break;
            }
          }

          /*
           * Plain titlebar presses are forwarded. This preserves toolkit CSD
           * behavior for dragging, minimize/maximize buttons, menus, tabs, etc.
           */
        }

        wlr_seat_pointer_notify_enter(server->seat,
                                      target_surface,
                                      sx,
                                      sy);

        wlr_seat_pointer_notify_motion(server->seat,
                                       time_msec,
                                       sx,
                                       sy);

        wlr_seat_pointer_notify_button(
          server->seat,
          time_msec,
          event->button,
          event->button_state == WD_POINTER_BUTTON_PRESSED
          ? WL_POINTER_BUTTON_STATE_PRESSED
          : WL_POINTER_BUTTON_STATE_RELEASED);
        break;

    case WD_POINTER_EVENT_AXIS: {
      enum wl_pointer_axis orientation =
          event->axis == WD_POINTER_AXIS_HORIZONTAL
              ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
              : WL_POINTER_AXIS_VERTICAL_SCROLL;

      wlr_seat_pointer_notify_axis(
          server->seat, time_msec, orientation, (double)event->axis_value,
          event->axis_value, WL_POINTER_AXIS_SOURCE_WHEEL,
          WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
      break;
    }

    default:
      break;
    }
  }
}
