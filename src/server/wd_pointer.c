#include "wd_server.h"

#include <string.h>

#include <wayland-server-protocol.h>

#include "waydisplay/wd_time.h"

#include <wlr/types/wlr_scene.h>

#define WD_FALLBACK_MOVE_ZONE_HEIGHT 32.0
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
           * Compositor fallback move:
           * Alt + left mouse drag moves the whole window.
           * Normal clicks must go through to the application.
           */
          if (event->button == WD_BTN_LEFT &&
            (event->modifiers & WD_POINTER_MOD_ALT)) {
            wd_pointer_begin_move(server, target_view);
          break;
            }
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
