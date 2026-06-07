#include "wd_server.h"

#if WAYDISPLAY_ENABLE_XWAYLAND

#include <stdlib.h>
#include <string.h>

enum wd_xwayland_decoration_part {
    WD_XWAYLAND_DECORATION_NONE = 0,
    WD_XWAYLAND_DECORATION_TITLEBAR,
    WD_XWAYLAND_DECORATION_MINIMIZE,
    WD_XWAYLAND_DECORATION_MAXIMIZE,
    WD_XWAYLAND_DECORATION_CLOSE,
};

static const float titlebar_color[4] = {0.14f, 0.16f, 0.18f, 1.0f};
static const float close_color[4]    = {0.80f, 0.18f, 0.16f, 1.0f};
static const float maximize_color[4] = {0.22f, 0.62f, 0.24f, 1.0f};
static const float minimize_color[4] = {0.86f, 0.66f, 0.18f, 1.0f};

static bool listener_is_linked(struct wl_listener* listener) {
    return listener && listener->link.prev && listener->link.next && listener->link.prev != &listener->link &&
           listener->link.next != &listener->link;
}

static void remove_listener_if_linked(struct wl_listener* listener) {
    if (!listener)
    {
        return;
    }

    if (listener_is_linked(listener))
    {
        wl_list_remove(&listener->link);
    }

    wl_list_init(&listener->link);
}

static char* dup_or_empty(const char* text) {
    if (!text)
    {
        text = "";
    }

    char* copy = strdup(text);
    return copy ? copy : strdup("");
}

static uint16_t sane_width(uint16_t width) {
    return width >= WD_XWAYLAND_MIN_VISIBLE_WIDTH ? width : WD_XWAYLAND_DEFAULT_WIDTH;
}

static uint16_t sane_height(uint16_t height) {
    return height >= WD_XWAYLAND_MIN_VISIBLE_HEIGHT ? height : WD_XWAYLAND_DEFAULT_HEIGHT;
}

bool wd_xwayland_view_has_decoration(struct wd_view* view) {
    return view && view->xwayland_surface && view->xwayland_had_map_request;
}

static uint16_t xwayland_content_width(struct wd_view* view) {
    if (!view || !view->xwayland_surface)
    {
        return WD_XWAYLAND_DEFAULT_WIDTH;
    }

    return sane_width(view->xwayland_surface->width);
}

static void xwayland_decoration_set_node_data(struct wlr_scene_rect* rect, struct wd_view* view) {
    if (rect)
    {
        rect->node.data = view;
    }
}

static void xwayland_view_update_decoration(struct wd_view* view) {
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    if (!wd_xwayland_view_has_decoration(view))
    {
        if (view->xwayland_surface_tree)
        {
            wlr_scene_node_set_position(&view->xwayland_surface_tree->node, 0, 0);
        }
        return;
    }

    if (!view->xwayland_titlebar_rect)
    {
        return;
    }

    uint16_t width = xwayland_content_width(view);

    wlr_scene_rect_set_size(view->xwayland_titlebar_rect, width, WD_XWAYLAND_TITLEBAR_HEIGHT);

    uint16_t button_y   = (uint16_t)((WD_XWAYLAND_TITLEBAR_HEIGHT - WD_XWAYLAND_BUTTON_SIZE) / 2u);
    uint16_t close_x    = width > WD_XWAYLAND_BUTTON_MARGIN + WD_XWAYLAND_BUTTON_SIZE
                              ? (uint16_t)(width - WD_XWAYLAND_BUTTON_MARGIN - WD_XWAYLAND_BUTTON_SIZE)
                              : 0u;
    uint16_t maximize_x = close_x > WD_XWAYLAND_BUTTON_GAP + WD_XWAYLAND_BUTTON_SIZE
                              ? (uint16_t)(close_x - WD_XWAYLAND_BUTTON_GAP - WD_XWAYLAND_BUTTON_SIZE)
                              : 0u;
    uint16_t minimize_x = maximize_x > WD_XWAYLAND_BUTTON_GAP + WD_XWAYLAND_BUTTON_SIZE
                              ? (uint16_t)(maximize_x - WD_XWAYLAND_BUTTON_GAP - WD_XWAYLAND_BUTTON_SIZE)
                              : 0u;

    if (view->xwayland_close_rect)
    {
        wlr_scene_rect_set_size(view->xwayland_close_rect, WD_XWAYLAND_BUTTON_SIZE, WD_XWAYLAND_BUTTON_SIZE);
        wlr_scene_node_set_position(&view->xwayland_close_rect->node, close_x, button_y);
    }

    if (view->xwayland_maximize_rect)
    {
        wlr_scene_rect_set_size(view->xwayland_maximize_rect, WD_XWAYLAND_BUTTON_SIZE, WD_XWAYLAND_BUTTON_SIZE);
        wlr_scene_node_set_position(&view->xwayland_maximize_rect->node, maximize_x, button_y);
    }

    if (view->xwayland_minimize_rect)
    {
        wlr_scene_rect_set_size(view->xwayland_minimize_rect, WD_XWAYLAND_BUTTON_SIZE, WD_XWAYLAND_BUTTON_SIZE);
        wlr_scene_node_set_position(&view->xwayland_minimize_rect->node, minimize_x, button_y);
    }

    if (view->xwayland_surface_tree)
    {
        wlr_scene_node_set_position(&view->xwayland_surface_tree->node, 0, WD_XWAYLAND_TITLEBAR_HEIGHT);
    }
}

static enum wd_xwayland_decoration_part xwayland_decoration_part_at(struct wd_view* view, double sx, double sy) {
    if (!wd_xwayland_view_has_decoration(view) || sx < 0.0 || sy < 0.0 || sy >= (double)WD_XWAYLAND_TITLEBAR_HEIGHT)
    {
        return WD_XWAYLAND_DECORATION_NONE;
    }

    double width      = (double)xwayland_content_width(view);
    double button_y   = (double)((WD_XWAYLAND_TITLEBAR_HEIGHT - WD_XWAYLAND_BUTTON_SIZE) / 2u);
    double close_x    = width - WD_XWAYLAND_BUTTON_MARGIN - WD_XWAYLAND_BUTTON_SIZE;
    double maximize_x = close_x - WD_XWAYLAND_BUTTON_GAP - WD_XWAYLAND_BUTTON_SIZE;
    double minimize_x = maximize_x - WD_XWAYLAND_BUTTON_GAP - WD_XWAYLAND_BUTTON_SIZE;

    if (sy >= button_y && sy < button_y + WD_XWAYLAND_BUTTON_SIZE)
    {
        if (sx >= close_x && sx < close_x + WD_XWAYLAND_BUTTON_SIZE)
        {
            return WD_XWAYLAND_DECORATION_CLOSE;
        }
        if (sx >= maximize_x && sx < maximize_x + WD_XWAYLAND_BUTTON_SIZE)
        {
            return WD_XWAYLAND_DECORATION_MAXIMIZE;
        }
        if (sx >= minimize_x && sx < minimize_x + WD_XWAYLAND_BUTTON_SIZE)
        {
            return WD_XWAYLAND_DECORATION_MINIMIZE;
        }
    }

    if (sx < width)
    {
        return WD_XWAYLAND_DECORATION_TITLEBAR;
    }

    return WD_XWAYLAND_DECORATION_NONE;
}

static void xwayland_view_update_metadata(struct wd_view* view) {
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    char* title  = dup_or_empty(view->xwayland_surface->title);
    char* app_id = dup_or_empty(view->xwayland_surface->class);

    if (title)
    {
        free(view->title);
        view->title = title;
    }

    if (app_id)
    {
        free(view->app_id);
        view->app_id = app_id;
    }
}

static void xwayland_view_clear_focus_and_grabs(struct wd_view* view) {
    if (!view || !view->server)
    {
        return;
    }

    struct wd_server* server = view->server;

    if (server->focused_view == view)
    {
        server->focused_view    = NULL;
        server->focused_surface = NULL;
        wd_keyboard_shortcuts_inhibit_refresh(server);

        if (server->seat)
        {
            wlr_seat_pointer_notify_clear_focus(server->seat);
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }

    wd_pointer_clear_button_grab_for_view(server, view);

    if (server->move_grab.view == view)
    {
        server->move_grab.active = false;
        server->move_grab.view   = NULL;
    }

    if (server->resize_grab.view == view)
    {
        server->resize_grab.active = false;
        server->resize_grab.view   = NULL;
    }
}

static void xwayland_view_configure_current_geometry(struct wd_view* view) {
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    struct wlr_xwayland_surface* xsurface = view->xwayland_surface;
    uint16_t                     width    = sane_width(xsurface->width);
    uint16_t                     height   = sane_height(xsurface->height);

    wlr_xwayland_surface_configure(xsurface, view->x, view->y, width, height);
    xwayland_view_update_decoration(view);
}

static void xwayland_view_save_geometry(struct wd_view* view) {
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    struct wlr_xwayland_surface* xsurface = view->xwayland_surface;

    view->saved_x      = view->x;
    view->saved_y      = view->y;
    view->saved_width  = sane_width(xsurface->width);
    view->saved_height = sane_height(xsurface->height);
}

static void xwayland_view_restore_saved_geometry(struct wd_view* view) {
    if (!view || !view->xwayland_surface || view->saved_width == 0 || view->saved_height == 0)
    {
        return;
    }

    view->x = view->saved_x;
    view->y = view->saved_y;
    wd_scene_set_view_position(view);

    wlr_xwayland_surface_configure(view->xwayland_surface, view->x, view->y, (uint16_t)view->saved_width, (uint16_t)view->saved_height);
    xwayland_view_update_decoration(view);
}

static uint16_t xwayland_view_display_width(struct wd_view* view) {
    double scale = view && view->server ? view->server->output_scale : 1.0;
    if (scale <= 0.0)
    {
        scale = 1.0;
    }

    return sane_width((uint16_t)((double)view->server->display_width / scale));
}

static uint16_t xwayland_view_display_height(struct wd_view* view) {
    double scale = view && view->server ? view->server->output_scale : 1.0;
    if (scale <= 0.0)
    {
        scale = 1.0;
    }

    uint16_t display_height = sane_height((uint16_t)((double)view->server->display_height / scale));
    if (display_height > WD_XWAYLAND_TITLEBAR_HEIGHT)
    {
        display_height = (uint16_t)(display_height - WD_XWAYLAND_TITLEBAR_HEIGHT);
    }

    return display_height;
}

static void xwayland_mark_scene_dirty(struct wd_view* view) {
    if (!view || !view->server)
    {
        return;
    }

    /*
     * Keep lifecycle events conservative: map/unmap/associate/dissociate can
     * create or remove scene nodes and can therefore affect arbitrary output
     * areas.
     */
    wd_server_mark_scene_dirty(view->server);
}

static void xwayland_mark_content_dirty(struct wd_view* view) {
    if (!view || !view->server)
    {
        return;
    }

    /*
     * Xwayland clients often commit repeatedly while reusing or cycling
     * equivalent buffers. Treat ordinary content commits as damage to the
     * Xwayland view bounds instead of forcing a full-output readback/hash pass
     * every time. This remains conservative for subwindows and redirected X11
     * drawing inside the window while avoiding redundant work in unrelated
     * parts of the output.
     */
    wd_server_mark_view_dirty(view);
}

static void xwayland_mark_configure_dirty(struct wd_view* view, int old_x, int old_y) {
    if (!view || !view->server)
    {
        return;
    }

    wd_server_mark_view_move_dirty(view, old_x, old_y);
}

static void xwayland_view_mark_mapped(struct wd_view* view, bool focus) {
    if (!view || !view->xwayland_surface || !view->server)
    {
        return;
    }

    xwayland_view_update_metadata(view);

    view->x         = view->xwayland_surface->x;
    view->y         = view->xwayland_surface->y;
    view->mapped    = true;
    view->minimized = false;

    wd_scene_set_view_position(view);
    xwayland_view_update_decoration(view);

    if (view->scene_tree)
    {
        if (focus && wd_xwayland_view_has_decoration(view))
        {
            wd_scene_focus_view(view);
        }
        else
        {
            wd_scene_raise_view(view);
        }
    }

    xwayland_mark_scene_dirty(view);
}

static void handle_xwayland_ready(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_server* server = wl_container_of(listener, server, xwayland_ready);

    if (!server || !server->xwayland)
    {
        return;
    }

    if (server->seat)
    {
        wlr_xwayland_set_seat(server->xwayland, server->seat);
    }

    if (server->xwayland->display_name)
    {
        setenv("DISPLAY", server->xwayland->display_name, 1);
        WD_LOG_INFO("WayDisplay: Xwayland ready on DISPLAY=%s", server->xwayland->display_name);
    }
    else
    {
        WD_LOG_INFO("WayDisplay: Xwayland ready");
    }
}

static void handle_xwayland_surface_commit(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_commit);

    if (view && view->server)
    {
        xwayland_view_update_decoration(view);
        xwayland_mark_content_dirty(view);
    }
}

static void handle_xwayland_surface_map(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_map);
    if (!view || !view->xwayland_surface || !view->server)
    {
        return;
    }

    xwayland_view_mark_mapped(view, true);

    WD_LOG_DEBUG("WayDisplay: Xwayland surface mapped view=%p geom=%dx%d+%d+%d title=%s class=%s", (void*)view,
                 (int)view->xwayland_surface->width, (int)view->xwayland_surface->height, (int)view->xwayland_surface->x,
                 (int)view->xwayland_surface->y, view->title ? view->title : "", view->app_id ? view->app_id : "");
}

static void handle_xwayland_surface_unmap(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_unmap);
    if (!view)
    {
        return;
    }

    view->mapped    = false;
    view->activated = false;
    xwayland_view_clear_focus_and_grabs(view);

    if (view->server)
    {
        xwayland_mark_scene_dirty(view);
    }
}

static void xwayland_view_disassociate(struct wd_view* view) {
    if (!view)
    {
        return;
    }

    remove_listener_if_linked(&view->xwayland_map);
    remove_listener_if_linked(&view->xwayland_unmap);
    remove_listener_if_linked(&view->xwayland_commit);

    if (view->scene_tree)
    {
        view->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree             = NULL;
        view->xwayland_surface_tree  = NULL;
        view->xwayland_titlebar_rect = NULL;
        view->xwayland_close_rect    = NULL;
        view->xwayland_maximize_rect = NULL;
        view->xwayland_minimize_rect = NULL;
    }

    view->mapped    = false;
    view->activated = false;
    xwayland_view_clear_focus_and_grabs(view);

    if (view->server)
    {
        xwayland_mark_scene_dirty(view);
    }
}

static void xwayland_view_attach_surface_listeners(struct wd_view* view) {
    if (!view || !view->xwayland_surface || !view->xwayland_surface->surface)
    {
        return;
    }

    struct wlr_surface* surface = view->xwayland_surface->surface;

    if (!listener_is_linked(&view->xwayland_map))
    {
        view->xwayland_map.notify = handle_xwayland_surface_map;
        wl_signal_add(&surface->events.map, &view->xwayland_map);
    }

    if (!listener_is_linked(&view->xwayland_unmap))
    {
        view->xwayland_unmap.notify = handle_xwayland_surface_unmap;
        wl_signal_add(&surface->events.unmap, &view->xwayland_unmap);
    }

    if (!listener_is_linked(&view->xwayland_commit))
    {
        view->xwayland_commit.notify = handle_xwayland_surface_commit;
        wl_signal_add(&surface->events.commit, &view->xwayland_commit);
    }
}

static void xwayland_view_associate(struct wd_view* view) {
    if (!view || !view->server || !view->server->scene || !view->xwayland_surface || !view->xwayland_surface->surface)
    {
        return;
    }

    if (!view->scene_tree)
    {
        view->scene_tree = wlr_scene_tree_create(&view->server->scene->tree);
        if (view->scene_tree)
        {
            view->scene_tree->node.data = view;
            if (wd_xwayland_view_has_decoration(view))
            {
                view->xwayland_titlebar_rect =
                    wlr_scene_rect_create(view->scene_tree, xwayland_content_width(view), WD_XWAYLAND_TITLEBAR_HEIGHT, titlebar_color);
                xwayland_decoration_set_node_data(view->xwayland_titlebar_rect, view);

                view->xwayland_minimize_rect =
                    wlr_scene_rect_create(view->scene_tree, WD_XWAYLAND_BUTTON_SIZE, WD_XWAYLAND_BUTTON_SIZE, minimize_color);
                xwayland_decoration_set_node_data(view->xwayland_minimize_rect, view);

                view->xwayland_maximize_rect =
                    wlr_scene_rect_create(view->scene_tree, WD_XWAYLAND_BUTTON_SIZE, WD_XWAYLAND_BUTTON_SIZE, maximize_color);
                xwayland_decoration_set_node_data(view->xwayland_maximize_rect, view);

                view->xwayland_close_rect =
                    wlr_scene_rect_create(view->scene_tree, WD_XWAYLAND_BUTTON_SIZE, WD_XWAYLAND_BUTTON_SIZE, close_color);
                xwayland_decoration_set_node_data(view->xwayland_close_rect, view);
            }

            view->xwayland_surface_tree = wlr_scene_subsurface_tree_create(view->scene_tree, view->xwayland_surface->surface);
            if (view->xwayland_surface_tree)
            {
                view->xwayland_surface_tree->node.data = view;
            }

            wd_scene_set_view_position(view);
            xwayland_view_update_decoration(view);
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
    if (view->mapped || view->xwayland_surface->surface->mapped)
    {
        xwayland_view_mark_mapped(view, true);
    }
    else
    {
        xwayland_mark_scene_dirty(view);
    }

    WD_LOG_DEBUG("WayDisplay: Xwayland associated view=%p scene_tree=%p mapped=%d", (void*)view, (void*)view->scene_tree,
                 view->mapped ? 1 : 0);
}

static void handle_xwayland_associate(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_associate);
    xwayland_view_associate(view);
}

static void handle_xwayland_dissociate(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_dissociate);
    xwayland_view_disassociate(view);
}

static void handle_xwayland_map_request(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_map_request);
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    view->xwayland_had_map_request = true;

    xwayland_view_mark_mapped(view, view->scene_tree != NULL);
    xwayland_view_configure_current_geometry(view);

    WD_LOG_DEBUG("WayDisplay: Xwayland map request view=%p requested=%dx%d+%d+%d configured=%ux%u "
                 "pending_associate=%d",
                 (void*)view, (int)view->xwayland_surface->width, (int)view->xwayland_surface->height, (int)view->xwayland_surface->x,
                 (int)view->xwayland_surface->y, (unsigned)sane_width(view->xwayland_surface->width),
                 (unsigned)sane_height(view->xwayland_surface->height), view->scene_tree ? 0 : 1);
}

static void xwayland_view_set_maximized(struct wd_view* view, bool maximize) {
    if (!view || !view->xwayland_surface || !view->server)
    {
        return;
    }

    struct wlr_xwayland_surface* xsurface = view->xwayland_surface;

    if (maximize && !view->maximized)
    {
        xwayland_view_save_geometry(view);

        view->x = 0;
        view->y = 0;
        wd_scene_set_view_position(view);

        wlr_xwayland_surface_configure(xsurface, view->x, view->y, xwayland_view_display_width(view), xwayland_view_display_height(view));
        xwayland_view_update_decoration(view);
    }
    else if (!maximize && view->maximized)
    {
        xwayland_view_restore_saved_geometry(view);
    }

    view->maximized   = maximize;
    view->minimized   = false;
    view->tiled_edges = 0;

    wlr_xwayland_surface_set_maximized(xsurface, maximize, maximize);
    wd_scene_focus_view(view);
    xwayland_mark_scene_dirty(view);
}

static void handle_xwayland_request_maximize(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_request_maximize);
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    bool maximize = view->xwayland_surface->maximized_horz && view->xwayland_surface->maximized_vert;
    xwayland_view_set_maximized(view, maximize);
}

static void handle_xwayland_request_fullscreen(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_request_fullscreen);
    if (!view || !view->xwayland_surface || !view->server)
    {
        return;
    }

    struct wlr_xwayland_surface* xsurface   = view->xwayland_surface;
    bool                         fullscreen = xsurface->fullscreen;

    if (fullscreen && !view->fullscreen)
    {
        xwayland_view_save_geometry(view);

        view->x = 0;
        view->y = 0;
        wd_scene_set_view_position(view);

        wlr_xwayland_surface_configure(xsurface, view->x, view->y, xwayland_view_display_width(view), xwayland_view_display_height(view));
        xwayland_view_update_decoration(view);
    }
    else if (!fullscreen && view->fullscreen)
    {
        xwayland_view_restore_saved_geometry(view);
    }

    view->fullscreen  = fullscreen;
    view->minimized   = false;
    view->tiled_edges = 0;

    wlr_xwayland_surface_set_fullscreen(xsurface, fullscreen);
    wd_scene_focus_view(view);
    xwayland_mark_scene_dirty(view);
}

static void handle_xwayland_request_minimize(struct wl_listener* listener, void* data) {
    struct wd_view*                     view  = wl_container_of(listener, view, xwayland_request_minimize);
    struct wlr_xwayland_minimize_event* event = data;

    if (!view || !view->xwayland_surface || !view->server)
    {
        return;
    }

    bool minimize = event ? event->minimize : view->xwayland_surface->minimized;

    view->minimized = minimize;
    wlr_xwayland_surface_set_minimized(view->xwayland_surface, minimize);

    if (minimize && view->server->focused_view == view)
    {
        view->server->focused_view    = NULL;
        view->server->focused_surface = NULL;
        wd_keyboard_shortcuts_inhibit_refresh(view->server);

        if (view->server->seat)
        {
            wlr_seat_pointer_notify_clear_focus(view->server->seat);
            wlr_seat_keyboard_notify_clear_focus(view->server->seat);
        }
    }
    else if (!minimize)
    {
        wd_scene_focus_view(view);
    }

    xwayland_mark_scene_dirty(view);
}

static void handle_xwayland_request_close(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_request_close);
    if (!view || !view->xwayland_surface)
    {
        return;
    }

    wlr_xwayland_surface_close(view->xwayland_surface);
}

static void handle_xwayland_request_configure(struct wl_listener* listener, void* data) {
    struct wd_view*                              view  = wl_container_of(listener, view, xwayland_request_configure);
    struct wlr_xwayland_surface_configure_event* event = data;

    if (!view || !view->xwayland_surface || !event)
    {
        return;
    }

    int old_x = view->x;
    int old_y = view->y;

    view->x = event->x;
    view->y = event->y;

    uint16_t width  = sane_width(event->width);
    uint16_t height = sane_height(event->height);

    wlr_xwayland_surface_configure(view->xwayland_surface, event->x, event->y, width, height);
    wd_scene_set_view_position(view);
    xwayland_view_update_decoration(view);

    if (view->server)
    {
        xwayland_mark_configure_dirty(view, old_x, old_y);
    }
}

static void handle_xwayland_surface_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xwayland_destroy);
    if (!view)
    {
        return;
    }

    struct wd_server* server = view->server;

    xwayland_view_clear_focus_and_grabs(view);

    remove_listener_if_linked(&view->xwayland_destroy);
    remove_listener_if_linked(&view->xwayland_associate);
    remove_listener_if_linked(&view->xwayland_dissociate);
    remove_listener_if_linked(&view->xwayland_map);
    remove_listener_if_linked(&view->xwayland_unmap);
    remove_listener_if_linked(&view->xwayland_commit);
    remove_listener_if_linked(&view->xwayland_map_request);
    remove_listener_if_linked(&view->xwayland_request_configure);
    remove_listener_if_linked(&view->xwayland_request_maximize);
    remove_listener_if_linked(&view->xwayland_request_fullscreen);
    remove_listener_if_linked(&view->xwayland_request_minimize);
    remove_listener_if_linked(&view->xwayland_request_close);

    if (view->link.prev && view->link.next)
    {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    if (view->scene_tree)
    {
        view->scene_tree->node.data = NULL;
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree             = NULL;
        view->xwayland_surface_tree  = NULL;
        view->xwayland_titlebar_rect = NULL;
        view->xwayland_close_rect    = NULL;
        view->xwayland_maximize_rect = NULL;
        view->xwayland_minimize_rect = NULL;
    }

    free(view->app_id);
    free(view->title);
    free(view);

    if (server)
    {
        wd_server_mark_scene_dirty(server);
    }
}

static void handle_new_xwayland_surface(struct wl_listener* listener, void* data) {
    struct wd_server*            server   = wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface* xsurface = data;

    if (!server || !xsurface || !server->scene)
    {
        return;
    }

    struct wd_view* view = calloc(1, sizeof(*view));
    if (!view)
    {
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
    wl_list_init(&view->xwayland_request_maximize.link);
    wl_list_init(&view->xwayland_request_fullscreen.link);
    wl_list_init(&view->xwayland_request_minimize.link);
    wl_list_init(&view->xwayland_request_close.link);

    view->server           = server;
    view->xwayland_surface = xsurface;
    view->x                = xsurface->x;
    view->y                = xsurface->y;
    view->positioned       = true;

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
    wl_signal_add(&xsurface->events.request_configure, &view->xwayland_request_configure);

    view->xwayland_request_maximize.notify = handle_xwayland_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize, &view->xwayland_request_maximize);

    view->xwayland_request_fullscreen.notify = handle_xwayland_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &view->xwayland_request_fullscreen);

    view->xwayland_request_minimize.notify = handle_xwayland_request_minimize;
    wl_signal_add(&xsurface->events.request_minimize, &view->xwayland_request_minimize);

    view->xwayland_request_close.notify = handle_xwayland_request_close;
    wl_signal_add(&xsurface->events.request_close, &view->xwayland_request_close);

    if (xsurface->surface)
    {
        xwayland_view_associate(view);
    }

    WD_LOG_DEBUG("WayDisplay: new Xwayland shell surface view=%p", (void*)view);
}

bool wd_xwayland_view_decoration_at(struct wd_view* view, double sx, double sy) {
    return xwayland_decoration_part_at(view, sx, sy) != WD_XWAYLAND_DECORATION_NONE;
}

bool wd_xwayland_view_handle_decoration_press(struct wd_view* view, double sx, double sy) {
    enum wd_xwayland_decoration_part part = xwayland_decoration_part_at(view, sx, sy);

    if (!view || !view->xwayland_surface || !view->server || part == WD_XWAYLAND_DECORATION_NONE)
    {
        return false;
    }

    wd_scene_focus_view(view);

    switch (part)
    {
    case WD_XWAYLAND_DECORATION_CLOSE:
        wlr_xwayland_surface_close(view->xwayland_surface);
        return true;
    case WD_XWAYLAND_DECORATION_MAXIMIZE:
        xwayland_view_set_maximized(view, !view->maximized);
        return true;
    case WD_XWAYLAND_DECORATION_MINIMIZE:
        view->minimized = true;
        wlr_xwayland_surface_set_minimized(view->xwayland_surface, true);
        xwayland_mark_scene_dirty(view);
        return true;
    case WD_XWAYLAND_DECORATION_TITLEBAR:
        wd_pointer_begin_move(view->server, view);
        return true;
    case WD_XWAYLAND_DECORATION_NONE:
        break;
    }

    return false;
}

bool wd_xwayland_init(struct wd_server* server) {
    if (!server || !server->display || !server->compositor)
    {
        return false;
    }

    server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
    if (!server->xwayland)
    {
        WD_LOG_ERROR("WayDisplay: failed to create Xwayland");
        return false;
    }

    wl_list_init(&server->xwayland_ready.link);
    wl_list_init(&server->new_xwayland_surface.link);

    server->xwayland_ready.notify = handle_xwayland_ready;
    wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);

    server->new_xwayland_surface.notify = handle_new_xwayland_surface;
    wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);

    if (server->xwayland->display_name)
    {
        WD_LOG_INFO("WayDisplay: Xwayland enabled on DISPLAY=%s", server->xwayland->display_name);
    }
    else
    {
        WD_LOG_INFO("WayDisplay: Xwayland enabled");
    }

    return true;
}

void wd_xwayland_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    remove_listener_if_linked(&server->xwayland_ready);
    remove_listener_if_linked(&server->new_xwayland_surface);

    if (server->xwayland)
    {
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }
}

#endif
