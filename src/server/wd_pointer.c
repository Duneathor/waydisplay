#include "waydisplay/wd_input.h"
#include "waydisplay/wd_time.h"
#include "wd_server_internal.h"

#include <string.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_scene.h>

#define WD_POINTER_MOD_ALT (1u << 0)

static bool scene_surface_at(struct wd_server* server, double lx, double ly, struct wd_view** out_view, struct wlr_surface** out_surface,
                             double* out_sx, double* out_sy) {
    if (out_view)
    {
        *out_view = NULL;
    }

    if (out_surface)
    {
        *out_surface = NULL;
    }

    if (out_sx)
    {
        *out_sx = 0.0;
    }

    if (out_sy)
    {
        *out_sy = 0.0;
    }

    if (!server || !server->scene)
    {
        return false;
    }

    double sx = 0.0;
    double sy = 0.0;

    struct wlr_scene_node* node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, &sx, &sy);

    if (!node)
    {
        return false;
    }

    struct wlr_scene_surface* scene_surface = NULL;

    if (node->type == WLR_SCENE_NODE_BUFFER)
    {
        struct wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);

        scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    }

    struct wd_view* view = wd_scene_view_from_node(server, node);

    if (!view)
    {
        return false;
    }

    if (out_view)
    {
        *out_view = view;
    }

    if (out_surface)
    {
        *out_surface = scene_surface ? scene_surface->surface : NULL;
    }

    if (out_sx)
    {
        *out_sx = sx;
    }

    if (out_sy)
    {
        *out_sy = sy;
    }

    return true;
}

/*
 * Client timestamps come from the client monotonic clock and cannot be compared
 * with the server monotonic clock when client and server are different hosts.
 */
static void wd_stats_note_pointer_input_inject_locked(struct wd_net_state* net, uint64_t server_rx_timestamp_ns,
                                                      uint64_t inject_timestamp_ns) {
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

void wd_pointer_queue_event_locked(struct wd_net_state* net, const struct wd_pointer_event_payload* event,
                                   uint64_t server_rx_timestamp_ns) {
    if (!net || !event)
    {
        return;
    }

    /*
     * Pointer motion can arrive far faster than the compositor tick which drains
     * this queue.  Only the newest pending motion position matters until a
     * button/axis event creates an ordering boundary, so collapse back-to-back
     * motion events instead of filling the queue and doing O(n) memmoves.
     */
    if (event->event_type == WD_POINTER_EVENT_MOTION && net->pointer_queue_count > 0)
    {
        struct wd_queued_pointer_event* last = &net->pointer_queue[net->pointer_queue_count - 1];

        if (last->event.event_type == WD_POINTER_EVENT_MOTION)
        {
            last->event                  = *event;
            last->server_rx_timestamp_ns = server_rx_timestamp_ns;
            net->stats.pointer_events_rx++;
            return;
        }
    }

    if (net->pointer_queue_count >= WD_SERVER_POINTER_QUEUE_CAPACITY)
    {
        memmove(&net->pointer_queue[0], &net->pointer_queue[1], (WD_SERVER_POINTER_QUEUE_CAPACITY - 1u) * sizeof(net->pointer_queue[0]));

        net->pointer_queue_count = WD_SERVER_POINTER_QUEUE_CAPACITY - 1u;
        net->stats.pointer_events_dropped++;
    }

    struct wd_queued_pointer_event* dst = &net->pointer_queue[net->pointer_queue_count++];
    dst->event                          = *event;
    dst->server_rx_timestamp_ns         = server_rx_timestamp_ns;

    net->stats.pointer_events_rx++;
}

static struct wlr_surface* view_root_surface(struct wd_view* view) {
    if (!view)
    {
        return NULL;
    }

    if (view->xdg_surface)
    {
        return view->xdg_surface->surface;
    }

#if WAYDISPLAY_ENABLE_XWAYLAND
    if (view->xwayland_surface)
    {
        return view->xwayland_surface->surface;
    }
#endif

    return NULL;
}

static uint32_t view_width(struct wd_view* view) {
    struct wlr_surface* surface = view_root_surface(view);
    if (!surface)
    {
        return view && view->server ? view->server->display_width : WD_DISPLAY_WIDTH;
    }

    int width = surface->current.width;
    if (width <= 0)
    {
        return view && view->server ? view->server->display_width : WD_DISPLAY_WIDTH;
    }

    return (uint32_t)width;
}

static uint32_t view_height(struct wd_view* view) {
    struct wlr_surface* surface = view_root_surface(view);
    if (!surface)
    {
        return view && view->server ? view->server->display_height : WD_DISPLAY_HEIGHT;
    }

    int height = surface->current.height;
    if (height <= 0)
    {
        return view && view->server ? view->server->display_height : WD_DISPLAY_HEIGHT;
    }

    return (uint32_t)height;
}

static uint32_t resize_edges_at_view_point(struct wd_view* view, double sx, double sy) {
    const double width  = (double)view_width(view);
    const double height = (double)view_height(view);
    uint32_t     edges  = WLR_EDGE_NONE;

    if (sx <= WD_RESIZE_EDGE_ZONE)
        edges |= WLR_EDGE_LEFT;
    if (sy <= WD_RESIZE_EDGE_ZONE)
        edges |= WLR_EDGE_TOP;
    if (sx >= width - WD_RESIZE_EDGE_ZONE)
        edges |= WLR_EDGE_RIGHT;
    if (sy >= height - WD_RESIZE_EDGE_ZONE)
        edges |= WLR_EDGE_BOTTOM;

    return edges;
}

static void wd_pointer_update_hover_cursor(struct wd_server* server, struct wd_view* view, double sx, double sy) {
    if (!server || !view)
    {
        return;
    }

    uint32_t edges = resize_edges_at_view_point(view, sx, sy);
    wd_cursor_set_shape(server, wd_cursor_shape_for_resize_edges(edges));
}

static bool pointer_event_is_left_press(const struct wd_pointer_event_payload* event) {
    return event && event->event_type == WD_POINTER_EVENT_BUTTON && event->button_state == WD_POINTER_BUTTON_PRESSED &&
           event->button == WD_INPUT_BUTTON_LEFT;
}

static bool pointer_event_is_alt_left_press(const struct wd_pointer_event_payload* event) {
    return pointer_event_is_left_press(event) && (event->modifiers & WD_POINTER_MOD_ALT);
}

static bool view_point_is_titlebar_move_zone(double sx, double sy) {
    /*
     * Do not compositor-resize from the fallback titlebar strip. CSD clients need
     * the press so they can emit xdg_toplevel.request_move, and WayDisplay also
     * historically treated this region as the fallback move zone.
     */
    return sx >= WD_RESIZE_EDGE_ZONE && sy <= WD_FALLBACK_MOVE_ZONE_HEIGHT;
}

static bool target_is_view_root_surface(struct wd_view* view, struct wlr_surface* surface) {
    return view && surface && view_root_surface(view) == surface;
}

static bool target_allows_compositor_fallback_gestures(struct wd_view* view, struct wlr_surface* surface) {
    return target_is_view_root_surface(view, surface);
}

static void notify_pointer_enter_if_needed(struct wd_server* server, struct wlr_surface* surface, double sx, double sy) {
    if (!server || !server->seat || !surface)
    {
        return;
    }

    struct wlr_surface* old_surface = server->seat->pointer_state.focused_surface;
    if (old_surface == surface)
    {
        return;
    }

    WD_LOG_DEBUG("pointer enter old_surface=%p new_surface=%p sx=%.1f sy=%.1f", old_surface ? (void*)old_surface : NULL,
                 surface ? (void*)surface : NULL, sx, sy);
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
}

void wd_pointer_clear_focus(struct wd_server* server) {
    if (!server || !server->seat)
    {
        return;
    }

    if (!server->seat->pointer_state.focused_surface)
    {
        return;
    }

    wlr_seat_pointer_clear_focus(server->seat);
    wlr_seat_pointer_notify_frame(server->seat);
}

static bool listener_is_linked(struct wl_listener* listener) {
    return listener && listener->link.prev && listener->link.next;
}

static void remove_listener_if_linked(struct wl_listener* listener) {
    if (!listener_is_linked(listener))
    {
        return;
    }

    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}

static void pointer_button_grab_surface_handle_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_server* server = wl_container_of(listener, server, pointer_button_grab_surface_destroy);
    if (!server)
    {
        return;
    }

    server->net.stats.pointer_button_grab_surface_destroyed++;
    wd_pointer_clear_button_grab(server);
}

static void pointer_button_grab_reset(struct wd_server* server, const char* reason, bool completed) {
    if (!server)
    {
        return;
    }

    const bool had_grab =
        server->pointer_button_grab_active || server->pointer_button_grab_count > 0 || server->pointer_button_grab_surface;

    if (had_grab)
    {
        WD_LOG_DEBUG("pointer button grab %s surface=%p view=%p count=%u buttons=0x%x", reason ? reason : "clear",
                     (void*)server->pointer_button_grab_surface, (void*)server->pointer_button_grab_view,
                     (unsigned)server->pointer_button_grab_count, (unsigned)server->pointer_button_grab_buttons);
        if (completed)
        {
            server->net.stats.pointer_button_grab_ended++;
        }
        else
        {
            server->net.stats.pointer_button_grab_cleared++;
        }
    }

    remove_listener_if_linked(&server->pointer_button_grab_surface_destroy);

    server->pointer_button_grab_active     = false;
    server->pointer_button_grab_view       = NULL;
    server->pointer_button_grab_surface    = NULL;
    server->pointer_button_grab_layout_x   = 0.0;
    server->pointer_button_grab_layout_y   = 0.0;
    server->pointer_button_grab_surface_sx = 0.0;
    server->pointer_button_grab_surface_sy = 0.0;
    server->pointer_button_grab_count      = 0;
    server->pointer_button_grab_buttons    = 0;
}

void wd_pointer_clear_button_grab(struct wd_server* server) {
    pointer_button_grab_reset(server, "clear", false);
}

void wd_pointer_clear_button_grab_for_view(struct wd_server* server, struct wd_view* view) {
    if (!server || !view || server->pointer_button_grab_view != view)
    {
        return;
    }

    pointer_button_grab_reset(server, "view gone", false);
}

void wd_pointer_clear_button_grab_for_surface(struct wd_server* server, struct wlr_surface* surface) {
    if (!server || !surface || server->pointer_button_grab_surface != surface)
    {
        return;
    }

    pointer_button_grab_reset(server, "surface gone", false);
}

static void pointer_button_grab_begin(struct wd_server* server, struct wd_view* view, struct wlr_surface* surface, double lx, double ly,
                                      double sx, double sy, uint32_t button) {
    if (!server || !surface)
    {
        return;
    }

    pointer_button_grab_reset(server, "replace", false);

    server->pointer_button_grab_active     = true;
    server->pointer_button_grab_view       = view;
    server->pointer_button_grab_surface    = surface;
    server->pointer_button_grab_layout_x   = lx;
    server->pointer_button_grab_layout_y   = ly;
    server->pointer_button_grab_surface_sx = sx;
    server->pointer_button_grab_surface_sy = sy;
    server->pointer_button_grab_count      = 0;
    server->pointer_button_grab_buttons    = button ? (1u << (button & 31u)) : 0;

    server->pointer_button_grab_surface_destroy.notify = pointer_button_grab_surface_handle_destroy;
    wl_signal_add(&surface->events.destroy, &server->pointer_button_grab_surface_destroy);

    server->net.stats.pointer_button_grab_started++;

    WD_LOG_DEBUG("pointer button grab begin surface=%p view=%p layout=%.1f %.1f sx=%.1f sy=%.1f button=0x%x", (void*)surface, (void*)view,
                 lx, ly, sx, sy, button);
}

void wd_pointer_begin_move(struct wd_server* server, struct wd_view* view) {
    if (!server || !view)
    {
        return;
    }

    if (view->fullscreen || view->maximized)
    {
        WD_LOG_DEBUG("ignoring move of %s view", view->fullscreen ? "fullscreen" : "maximized");
        return;
    }

    server->move_grab.active = true;
    server->move_grab.view   = view;
    server->move_grab.grab_x = server->pointer_x;
    server->move_grab.grab_y = server->pointer_y;
    server->move_grab.view_x = view->x;
    server->move_grab.view_y = view->y;
    wd_cursor_set_shape(server, WD_CURSOR_SHAPE_MOVE);

    WD_LOG_DEBUG("begin move view=%p at pointer %.1f %.1f view=%d %d", (void*)view, server->pointer_x, server->pointer_y, view->x, view->y);
}

void wd_pointer_update_move(struct wd_server* server) {
    if (!server || !server->move_grab.active || !server->move_grab.view)
    {
        return;
    }

    struct wd_view* view = server->move_grab.view;

    const double dx = server->pointer_x - server->move_grab.grab_x;
    const double dy = server->pointer_y - server->move_grab.grab_y;

    int old_x = view->x;
    int old_y = view->y;

    view->x = server->move_grab.view_x + (int)dx;
    view->y = server->move_grab.view_y + (int)dy;

    wd_scene_set_view_position(view);
    wd_server_mark_view_move_dirty(view, old_x, old_y);
}

void wd_pointer_end_move(struct wd_server* server) {
    if (!server || !server->move_grab.active)
    {
        return;
    }

    WD_LOG_DEBUG("end move");

    if (server->move_grab.view)
    {
        wd_server_mark_view_dirty(server->move_grab.view);
    }

    server->move_grab.active = false;
    server->move_grab.view   = NULL;
}

void wd_pointer_begin_resize(struct wd_server* server, struct wd_view* view, uint32_t edges) {
    if (!server || !view || edges == WLR_EDGE_NONE)
    {
        return;
    }

    if (view->fullscreen || view->maximized)
    {
        WD_LOG_DEBUG("ignoring resize of %s view", view->fullscreen ? "fullscreen" : "maximized");
        return;
    }

    if (!view->xdg_surface || !view->xdg_surface->toplevel)
    {
#if WAYDISPLAY_ENABLE_XWAYLAND
        if (!view->xwayland_surface)
        {
            return;
        }
#else
        return;
#endif
    }

    server->resize_grab.active      = true;
    server->resize_grab.view        = view;
    server->resize_grab.edges       = edges;
    server->resize_grab.grab_x      = server->pointer_x;
    server->resize_grab.grab_y      = server->pointer_y;
    server->resize_grab.view_x      = view->x;
    server->resize_grab.view_y      = view->y;
    server->resize_grab.view_width  = view_width(view);
    server->resize_grab.view_height = view_height(view);
    wd_cursor_set_shape(server, wd_cursor_shape_for_resize_edges(edges));

    if (view->xdg_surface && view->xdg_surface->toplevel)
    {
        wlr_xdg_toplevel_set_resizing(view->xdg_surface->toplevel, true);
    }

    WD_LOG_DEBUG("begin resize view=%p edges=0x%x pointer %.1f %.1f "
                 "view=%d %d size=%ux%u",
                 (void*)view, edges, server->pointer_x, server->pointer_y, view->x, view->y, server->resize_grab.view_width,
                 server->resize_grab.view_height);
}

void wd_pointer_update_resize(struct wd_server* server) {
    if (!server || !server->resize_grab.active || !server->resize_grab.view)
    {
        return;
    }

    struct wd_view* view = server->resize_grab.view;

    if (!view->xdg_surface || !view->xdg_surface->toplevel)
    {
#if WAYDISPLAY_ENABLE_XWAYLAND
        if (!view->xwayland_surface)
        {
            return;
        }
#else
        return;
#endif
    }

    const double   dx    = server->pointer_x - server->resize_grab.grab_x;
    const double   dy    = server->pointer_y - server->resize_grab.grab_y;
    const uint32_t edges = server->resize_grab.edges;

    int new_x      = server->resize_grab.view_x;
    int new_y      = server->resize_grab.view_y;
    int new_width  = (int)server->resize_grab.view_width;
    int new_height = (int)server->resize_grab.view_height;

    if (edges & WLR_EDGE_LEFT)
    {
        new_width = (int)server->resize_grab.view_width - (int)dx;
        new_x     = server->resize_grab.view_x + (int)dx;

        if (new_width < (int)WD_MIN_WINDOW_WIDTH)
        {
            new_x -= (int)WD_MIN_WINDOW_WIDTH - new_width;
            new_width = (int)WD_MIN_WINDOW_WIDTH;
        }
    }

    if (edges & WLR_EDGE_RIGHT)
    {
        new_width = (int)server->resize_grab.view_width + (int)dx;

        if (new_width < (int)WD_MIN_WINDOW_WIDTH)
        {
            new_width = (int)WD_MIN_WINDOW_WIDTH;
        }
    }

    if (edges & WLR_EDGE_TOP)
    {
        new_height = (int)server->resize_grab.view_height - (int)dy;
        new_y      = server->resize_grab.view_y + (int)dy;

        if (new_height < (int)WD_MIN_WINDOW_HEIGHT)
        {
            new_y -= (int)WD_MIN_WINDOW_HEIGHT - new_height;
            new_height = (int)WD_MIN_WINDOW_HEIGHT;
        }
    }

    if (edges & WLR_EDGE_BOTTOM)
    {
        new_height = (int)server->resize_grab.view_height + (int)dy;

        if (new_height < (int)WD_MIN_WINDOW_HEIGHT)
        {
            new_height = (int)WD_MIN_WINDOW_HEIGHT;
        }
    }

    int old_x = view->x;
    int old_y = view->y;

    view->x = new_x;
    view->y = new_y;

    wd_scene_set_view_position(view);
    if (view->xdg_surface && view->xdg_surface->toplevel)
    {
        wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, (uint32_t)new_width, (uint32_t)new_height);
    }
#if WAYDISPLAY_ENABLE_XWAYLAND
    if (view->xwayland_surface)
    {
        wlr_xwayland_surface_configure(view->xwayland_surface, new_x, new_y, (uint32_t)new_width, (uint32_t)new_height);
    }
#endif

    wd_server_mark_view_move_dirty(view, old_x, old_y);
}

void wd_pointer_end_resize(struct wd_server* server) {
    if (!server || !server->resize_grab.active)
    {
        return;
    }

    if (server->resize_grab.view && server->resize_grab.view->xdg_surface && server->resize_grab.view->xdg_surface->toplevel)
    {
        wlr_xdg_toplevel_set_resizing(server->resize_grab.view->xdg_surface->toplevel, false);
    }

    WD_LOG_DEBUG("end resize");

    if (server->resize_grab.view)
    {
        wd_server_mark_view_dirty(server->resize_grab.view);
    }

    server->resize_grab.active = false;
    server->resize_grab.view   = NULL;
    server->resize_grab.edges  = WLR_EDGE_NONE;
}

static double clamp_layout_x(struct wd_server* server, uint16_t x) {
    const uint32_t width = server ? server->display_width : WD_DISPLAY_WIDTH;

    if (x >= width)
    {
        return (double)(width - 1);
    }

    return (double)x;
}

static double clamp_layout_y(struct wd_server* server, uint16_t y) {
    const uint32_t height = server ? server->display_height : WD_DISPLAY_HEIGHT;

    if (y >= height)
    {
        return (double)(height - 1);
    }

    return (double)y;
}

void wd_pointer_drain_and_inject(struct wd_server* server) {
    struct wd_queued_pointer_event local[WD_SERVER_POINTER_QUEUE_CAPACITY];
    size_t                         count = 0;

    if (!server || !server->seat)
    {
        return;
    }

    pthread_mutex_lock(&server->net.lock);

    count = server->net.pointer_queue_count;

    if (count > WD_SERVER_POINTER_QUEUE_CAPACITY)
    {
        count = WD_SERVER_POINTER_QUEUE_CAPACITY;
    }

    if (count > 0)
    {
        memcpy(local, server->net.pointer_queue, count * sizeof(local[0]));

        server->net.pointer_queue_count = 0;
    }

    pthread_mutex_unlock(&server->net.lock);

    if (count == 0)
    {
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const struct wd_pointer_event_payload* event = &local[i].event;

        const uint64_t inject_ns = wd_now_ns();
        const double   lx        = clamp_layout_x(server, event->x);
        const double   ly        = clamp_layout_y(server, event->y);

        pthread_mutex_lock(&server->net.lock);
        server->net.last_input_sequence = event->input_sequence;
        wd_stats_note_pointer_input_inject_locked(&server->net, local[i].server_rx_timestamp_ns, inject_ns);
        server->net.stats.pointer_events_injected++;
        pthread_mutex_unlock(&server->net.lock);

        const uint32_t time_msec =
            local[i].server_rx_timestamp_ns ? (uint32_t)(local[i].server_rx_timestamp_ns / 1000000ull) : (uint32_t)(inject_ns / 1000000ull);

        server->pointer_x = lx;
        server->pointer_y = ly;

        static struct wlr_surface* last_logged_motion_surface = NULL;
        static struct wd_view*     last_logged_motion_view    = NULL;
        static int                 last_logged_motion_x       = -1;
        static int                 last_logged_motion_y       = -1;
        static uint64_t            last_motion_log_ns         = 0;

#define WD_CLEAR_POINTER_BUTTON_GRAB() wd_pointer_clear_button_grab(server)

        /*
         * If the compositor is currently moving a window, pointer motion updates
         * the scene position and is not forwarded to the client surface.
         */
        if (server->move_grab.active)
        {
            if (event->event_type == WD_POINTER_EVENT_MOTION)
            {
                wd_pointer_update_move(server);
                continue;
            }

            if (event->event_type == WD_POINTER_EVENT_BUTTON && event->button_state == WD_POINTER_BUTTON_RELEASED)
            {
                wd_pointer_update_move(server);
                wd_pointer_end_move(server);
                WD_CLEAR_POINTER_BUTTON_GRAB();
                wd_pointer_clear_focus(server);
                continue;
            }

            continue;
        }

        /*
         * If the compositor is resizing a window, pointer motion updates the
         * requested toplevel size and is not forwarded to the client surface.
         */
        if (server->resize_grab.active)
        {
            if (event->event_type == WD_POINTER_EVENT_MOTION)
            {
                wd_pointer_update_resize(server);
                continue;
            }

            if (event->event_type == WD_POINTER_EVENT_BUTTON && event->button_state == WD_POINTER_BUTTON_RELEASED &&
                event->button == WD_INPUT_BUTTON_LEFT)
            {
                wd_pointer_update_resize(server);
                wd_pointer_end_resize(server);
                WD_CLEAR_POINTER_BUTTON_GRAB();
                wd_pointer_clear_focus(server);
                continue;
            }

            continue;
        }

        double sx = 0.0;
        double sy = 0.0;

        struct wd_view*     target_view    = NULL;
        struct wlr_surface* target_surface = NULL;

        bool hit_surface = scene_surface_at(server, lx, ly, &target_view, &target_surface, &sx, &sy);

        if (server->pointer_button_grab_count > 0 && server->pointer_button_grab_surface)
        {
            target_view    = server->pointer_button_grab_view;
            target_surface = server->pointer_button_grab_surface;
            sx             = server->pointer_button_grab_surface_sx + (lx - server->pointer_button_grab_layout_x);
            sy             = server->pointer_button_grab_surface_sy + (ly - server->pointer_button_grab_layout_y);
            hit_surface    = true;
        }

#if WAYDISPLAY_ENABLE_XWAYLAND
        if (hit_surface && target_view && !target_surface && target_view->xwayland_surface)
        {
            sx = lx - target_view->x;
            sy = ly - target_view->y;

            if (!wd_xwayland_view_decoration_at(target_view, sx, sy))
            {
                hit_surface = false;
            }
        }
#endif

        if (!hit_surface)
        {
            if (event->event_type == WD_POINTER_EVENT_MOTION)
            {
                const uint64_t now_ns         = wd_now_ns();
                const int      ilx            = (int)lx;
                const int      ily            = (int)ly;
                const bool     target_changed = last_logged_motion_surface != NULL || last_logged_motion_view != NULL;
                const bool     moved_enough   = last_logged_motion_x < 0 || last_logged_motion_y < 0 || ilx < last_logged_motion_x - 24 ||
                                                ilx > last_logged_motion_x + 24 || ily < last_logged_motion_y - 24 ||
                                                ily > last_logged_motion_y + 24;
                const bool     old_enough     = now_ns - last_motion_log_ns > 250000000ull;

                if (target_changed || (moved_enough && old_enough))
                {
                    WD_LOG_DEBUG("pointer motion target none at layout %.1f %.1f", lx, ly);
                    last_logged_motion_surface = NULL;
                    last_logged_motion_view    = NULL;
                    last_logged_motion_x       = ilx;
                    last_logged_motion_y       = ily;
                    last_motion_log_ns         = now_ns;
                }
            }

            if (event->event_type == WD_POINTER_EVENT_BUTTON && event->button == WD_INPUT_BUTTON_RIGHT)
            {
                WD_LOG_DEBUG("right click %s had no target at layout %.1f %.1f "
                             "mods=0x%x",
                             event->button_state == WD_POINTER_BUTTON_PRESSED ? "press" : "release", lx, ly, event->modifiers);
            }
            wd_cursor_set_shape(server, WD_CURSOR_SHAPE_DEFAULT);
            wd_pointer_clear_focus(server);
            continue;
        }

        if (event->event_type == WD_POINTER_EVENT_MOTION)
        {
            const uint64_t now_ns         = wd_now_ns();
            const int      ilx            = (int)lx;
            const int      ily            = (int)ly;
            const bool     target_changed = target_surface != last_logged_motion_surface || target_view != last_logged_motion_view;
            const bool moved_enough = last_logged_motion_x < 0 || last_logged_motion_y < 0 || ilx < last_logged_motion_x - 24 ||
                                      ilx > last_logged_motion_x + 24 || ily < last_logged_motion_y - 24 || ily > last_logged_motion_y + 24;
            const bool old_enough   = now_ns - last_motion_log_ns > 250000000ull;

            if (target_changed || (moved_enough && old_enough))
            {
                WD_LOG_DEBUG("pointer motion target layout %.1f %.1f "
                             "surface=%p view=%p sx=%.1f sy=%.1f",
                             lx, ly, (void*)target_surface, (void*)target_view, sx, sy);
                last_logged_motion_surface = target_surface;
                last_logged_motion_view    = target_view;
                last_logged_motion_x       = ilx;
                last_logged_motion_y       = ily;
                last_motion_log_ns         = now_ns;
            }
        }

        if (event->event_type == WD_POINTER_EVENT_BUTTON && event->button == WD_INPUT_BUTTON_RIGHT)
        {
            WD_LOG_DEBUG("right click target %s at layout %.1f %.1f "
                         "surface=%p view=%p sx=%.1f sy=%.1f state=%s mods=0x%x",
                         event->button_state == WD_POINTER_BUTTON_PRESSED ? "press" : "release", lx, ly, (void*)target_surface,
                         (void*)target_view, sx, sy, event->button_state == WD_POINTER_BUTTON_PRESSED ? "pressed" : "released",
                         event->modifiers);
        }

#if WAYDISPLAY_ENABLE_XWAYLAND
        if (!target_surface && target_view && target_view->xwayland_surface)
        {
            switch (event->event_type)
            {
            case WD_POINTER_EVENT_MOTION:
                wd_cursor_set_shape(server, WD_CURSOR_SHAPE_DEFAULT);
                break;
            case WD_POINTER_EVENT_BUTTON:
                if (event->button == WD_INPUT_BUTTON_LEFT && event->button_state == WD_POINTER_BUTTON_PRESSED)
                {
                    wd_xwayland_view_handle_decoration_press(target_view, sx, sy);
                }
                break;
            case WD_POINTER_EVENT_AXIS:
                break;
            }

            continue;
        }
#endif

        switch (event->event_type)
        {
        case WD_POINTER_EVENT_MOTION:
            if (target_allows_compositor_fallback_gestures(target_view, target_surface))
            {
                wd_pointer_update_hover_cursor(server, target_view, sx, sy);
            }
            notify_pointer_enter_if_needed(server, target_surface, sx, sy);

            wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
            wlr_seat_pointer_notify_frame(server->seat);
            break;

        case WD_POINTER_EVENT_BUTTON: {
            /*
             * Always deliver child-surface/subsurface/popup clicks to the actual
             * surface. Fallback compositor gestures are only valid on the root
             * xdg-toplevel surface. Toolkits such as Qt/KDE often create helper
             * wl_subsurface objects for complex widgets; stealing those clicks can
             * put the client into a bad state.
             */
            bool allow_fallback_gestures = target_allows_compositor_fallback_gestures(target_view, target_surface);

            if (event->button_state == WD_POINTER_BUTTON_PRESSED)
            {
                wd_scene_focus_view(target_view);

                if (allow_fallback_gestures && pointer_event_is_alt_left_press(event))
                {
                    WD_CLEAR_POINTER_BUTTON_GRAB();
                    wd_pointer_begin_move(server, target_view);
                    break;
                }

                /*
                 * Compositor fallback resize:
                 * Only on the root toplevel surface. Do not run this against
                 * subsurfaces or helper surfaces, because their local sx/sy are not
                 * root-window coordinates and their clicks belong to the toolkit.
                 */
                if (allow_fallback_gestures && pointer_event_is_left_press(event) && !view_point_is_titlebar_move_zone(sx, sy))
                {
                    uint32_t edges = resize_edges_at_view_point(target_view, sx, sy);
                    if (edges != WLR_EDGE_NONE)
                    {
                        WD_CLEAR_POINTER_BUTTON_GRAB();
                        wd_pointer_begin_resize(server, target_view, edges);
                        break;
                    }
                }
                else if (allow_fallback_gestures)
                {
                    wd_pointer_update_hover_cursor(server, target_view, sx, sy);
                }

                /*
                 * Plain titlebar presses are forwarded. This preserves toolkit CSD
                 * behavior for dragging, minimize/maximize buttons, menus, tabs, etc.
                 *
                 * Child-surface presses are also forwarded unchanged.
                 */
                if (server->pointer_button_grab_count == 0)
                {
                    pointer_button_grab_begin(server, target_view, target_surface, lx, ly, sx, sy, event->button);
                }

                ++server->pointer_button_grab_count;
                server->pointer_button_grab_buttons |= event->button ? (1u << (event->button & 31u)) : 0;
            }

            notify_pointer_enter_if_needed(server, target_surface, sx, sy);

            wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);

            if (event->button == WD_INPUT_BUTTON_RIGHT)
            {
                WD_LOG_DEBUG("notifying seat right click %s "
                             "button=0x%x time=%u surface=%p sx=%.1f sy=%.1f",
                             event->button_state == WD_POINTER_BUTTON_PRESSED ? "press" : "release", event->button, time_msec,
                             (void*)target_surface, sx, sy);
            }

            wlr_seat_pointer_notify_button(server->seat, time_msec, event->button,
                                           event->button_state == WD_POINTER_BUTTON_PRESSED ? WL_POINTER_BUTTON_STATE_PRESSED
                                                                                            : WL_POINTER_BUTTON_STATE_RELEASED);
            wlr_seat_pointer_notify_frame(server->seat);

            if (event->button_state == WD_POINTER_BUTTON_RELEASED && server->pointer_button_grab_count > 0)
            {
                --server->pointer_button_grab_count;
                if (event->button)
                {
                    server->pointer_button_grab_buttons &= ~(1u << (event->button & 31u));
                }

                if (server->pointer_button_grab_count == 0)
                {
                    pointer_button_grab_reset(server, "end", true);
                }
            }

            break;
        }

        case WD_POINTER_EVENT_AXIS: {
            enum wl_pointer_axis orientation =
                event->axis == WD_POINTER_AXIS_HORIZONTAL ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;

            wlr_seat_pointer_notify_axis(server->seat, time_msec, orientation, (double)event->axis_value, event->axis_value,
                                         WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            wlr_seat_pointer_notify_frame(server->seat);
            break;
        }

        default:
            break;
        }
    }

#undef WD_CLEAR_POINTER_BUTTON_GRAB
}
