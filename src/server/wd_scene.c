#include "wd_server.h"

#include <stdlib.h>
#include <string.h>

static void                view_configure_idle(void* data);
static void                view_schedule_initial_configure(struct wd_view* view);
static void                view_handle_commit(struct wl_listener* listener, void* data);
static void                view_handle_map(struct wl_listener* listener, void* data);
static void                view_handle_unmap(struct wl_listener* listener, void* data);
static void                view_apply_fractional_scale(struct wd_view* view);
static void                view_track_surface_commits(struct wd_view* view);
static void                view_surface_commit_trackers_destroy(struct wd_view* view);
static void                view_handle_xdg_surface_destroy(struct wl_listener* listener, void* data);
static void                view_handle_xdg_toplevel_destroy(struct wl_listener* listener, void* data);
static void                server_handle_new_xdg_surface(struct wl_listener* listener, void* data);
static void                server_handle_new_xdg_toplevel(struct wl_listener* listener, void* data);
static void                server_handle_new_xdg_popup(struct wl_listener* listener, void* data);
static void                view_handle_set_parent(struct wl_listener* listener, void* data);
static void                view_handle_set_app_id(struct wl_listener* listener, void* data);
static void                view_handle_set_title(struct wl_listener* listener, void* data);
static void                view_handle_request_move(struct wl_listener* listener, void* data);
static void                view_handle_request_resize(struct wl_listener* listener, void* data);
static void                view_handle_request_maximize(struct wl_listener* listener, void* data);
static void                view_handle_request_fullscreen(struct wl_listener* listener, void* data);
static void                view_handle_request_minimize(struct wl_listener* listener, void* data);
static void                server_clear_focus_if_view(struct wd_server* server, struct wd_view* view, bool clear_seat_focus);
static void                view_handle_new_popup(struct wl_listener* listener, void* data);
static struct wlr_surface* view_root_surface(struct wd_view* view);

struct wd_popup_unconstrain {
    struct wl_listener      popup_destroy;
    struct wl_listener      popup_surface_destroy;
    struct wl_listener      view_destroy;
    struct wl_event_source* idle;
    struct wlr_xdg_popup*   popup;
    struct wd_view*         view;
};

struct wd_popup_commit_tracker {
    struct wl_list         link;
    struct wl_listener     commit;
    struct wl_listener     map;
    struct wl_listener     unmap;
    struct wl_listener     destroy;
    struct wl_listener     view_destroy;
    struct wlr_xdg_popup*  popup;
    struct wd_view*        view;
    struct wlr_scene_tree* scene_tree;
    int                    have_last_geometry;
    int                    last_surface_width;
    int                    last_surface_height;
    int                    last_geometry_x;
    int                    last_geometry_y;
    int                    last_geometry_width;
    int                    last_geometry_height;
};

struct wd_surface_commit_tracker {
    struct wl_list      link;
    struct wl_listener  commit;
    struct wl_listener  destroy;
    struct wl_listener  view_destroy;
    struct wlr_surface* surface;
    struct wd_view*     view;
};

static void popup_unconstrain_idle(void* data);
static void popup_unconstrain_handle_popup_destroy(struct wl_listener* listener, void* data);
static void popup_unconstrain_handle_popup_surface_destroy(struct wl_listener* listener, void* data);
static void popup_unconstrain_handle_view_destroy(struct wl_listener* listener, void* data);
static void popup_commit_tracker_handle_commit(struct wl_listener* listener, void* data);
static void popup_commit_tracker_handle_map(struct wl_listener* listener, void* data);
static void popup_commit_tracker_handle_unmap(struct wl_listener* listener, void* data);
static void popup_commit_tracker_handle_destroy(struct wl_listener* listener, void* data);
static void popup_commit_tracker_handle_view_destroy(struct wl_listener* listener, void* data);
static bool popup_commit_tracker_ensure_scene_tree(struct wd_popup_commit_tracker* state);
static void surface_commit_tracker_handle_commit(struct wl_listener* listener, void* data);
static void surface_commit_tracker_handle_destroy(struct wl_listener* listener, void* data);
static void surface_commit_tracker_handle_view_destroy(struct wl_listener* listener, void* data);

void wd_scene_init_listeners(struct wd_server* server) {
    server->new_xdg_surface.notify = server_handle_new_xdg_surface;
    wl_signal_add(&server->xdg_shell->events.new_surface, &server->new_xdg_surface);

    server->new_xdg_toplevel.notify = server_handle_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

    server->new_xdg_popup.notify = server_handle_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);
}

struct wd_view* wd_scene_view_at(struct wd_server* server, double lx, double ly, double* sx, double* sy) {
    if (!server)
    {
        return NULL;
    }

    struct wd_view* view;

    /*
     * Tail is topmost because wd_scene_raise_view() and new-window insertion
     * should insert at server->views.prev.
     */
    wl_list_for_each_reverse(view, &server->views, link) {
        struct wlr_surface* surface = view_root_surface(view);
        if (!view->mapped || !surface)
        {
            continue;
        }

        int surface_w = surface->current.width;
        int surface_h = surface->current.height;

#if WAYDISPLAY_ENABLE_XWAYLAND
        if (wd_xwayland_view_has_decoration(view))
        {
            surface_h += WD_XWAYLAND_TITLEBAR_HEIGHT;
        }
#endif

        if (surface_w <= 0)
        {
            surface_w = (int)server->display_width;
        }

        if (surface_h <= 0)
        {
            surface_h = (int)server->display_height;
        }

        if (lx < view->x || ly < view->y || lx >= view->x + surface_w || ly >= view->y + surface_h)
        {
            continue;
        }

        if (sx)
        {
            *sx = lx - view->x;
        }

        if (sy)
        {
            *sy = ly - view->y;
        }

        return view;
    }

    return NULL;
}

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

void wd_scene_set_view_position(struct wd_view* view) {
    if (!view || !view->scene_tree)
    {
        return;
    }

    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
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

static char* dup_or_empty(const char* text) {
    if (!text)
    {
        text = "";
    }

    char* copy = strdup(text);
    return copy ? copy : strdup("");
}

static void view_update_app_id(struct wd_view* view) {
    if (!view || !view->xdg_surface || !view->xdg_surface->toplevel)
    {
        return;
    }

    char* copy = dup_or_empty(view->xdg_surface->toplevel->app_id);
    if (!copy)
    {
        return;
    }

    free(view->app_id);
    view->app_id = copy;
}

static void view_update_title(struct wd_view* view) {
    if (!view || !view->xdg_surface || !view->xdg_surface->toplevel)
    {
        return;
    }

    char* copy = dup_or_empty(view->xdg_surface->toplevel->title);
    if (!copy)
    {
        return;
    }

    free(view->title);
    view->title = copy;
}

static void view_update_metadata(struct wd_view* view) {
    view_update_app_id(view);
    view_update_title(view);
}

static const char* view_app_id(struct wd_view* view) {
    return view && view->app_id && view->app_id[0] ? view->app_id : "(no app-id)";
}

static const char* view_title(struct wd_view* view) {
    return view && view->title && view->title[0] ? view->title : "(no title)";
}

static bool xdg_surface_can_configure(struct wlr_xdg_surface* xdg_surface) {
    return xdg_surface && xdg_surface->initialized;
}

static bool xdg_toplevel_can_configure(struct wd_view* view) {
    return view && xdg_surface_can_configure(view->xdg_surface) && view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
           view->xdg_surface->toplevel;
}

static void view_set_activated(struct wd_view* view, bool activated) {
    if (!view)
    {
        return;
    }

    if (view->activated == activated)
    {
        return;
    }

    view->activated = activated;

    if (xdg_toplevel_can_configure(view))
    {
        wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, activated);
    }

#if WAYDISPLAY_ENABLE_XWAYLAND
    if (view->xwayland_surface)
    {
        wlr_xwayland_surface_activate(view->xwayland_surface, activated);
    }
#endif
}

static void server_clear_focus_if_view(struct wd_server* server, struct wd_view* view, bool clear_seat_focus) {
    if (!server || !view || server->focused_view != view)
    {
        return;
    }

    server->focused_view    = NULL;
    server->focused_surface = NULL;

    wd_keyboard_shortcuts_inhibit_refresh(server);

    if (clear_seat_focus && server->seat)
    {
        wlr_seat_pointer_notify_clear_focus(server->seat);
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
}

void wd_scene_deactivate_view(struct wd_view* view) {
    if (!view)
    {
        return;
    }

    view_set_activated(view, false);

    if (view->server)
    {
        server_clear_focus_if_view(view->server, view, true);
    }
}

static void view_set_bounds(struct wd_view* view, uint32_t width, uint32_t height) {
    if (!view)
    {
        return;
    }

    view->bounds_width  = width;
    view->bounds_height = height;

    if (!xdg_toplevel_can_configure(view))
    {
        return;
    }

    wlr_xdg_toplevel_set_bounds(view->xdg_surface->toplevel, width, height);
}

static void view_mark_geometry_before_change(struct wd_view* view) {
    if (!view || !view->mapped)
    {
        return;
    }

    wd_server_mark_view_dirty(view);
}

static void view_mark_geometry_after_change(struct wd_view* view) {
    if (!view || !view->mapped)
    {
        return;
    }

    wd_server_mark_view_dirty(view);
}

static int clamp_i(int value, int min_value, int max_value) {
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

static uint32_t output_logical_width(struct wd_server* server) {
    double scale = server ? server->output_scale : 1.0;
    if (scale <= 0.0)
    {
        scale = 1.0;
    }

    uint32_t width = (uint32_t)((double)(server ? server->display_width : WD_DISPLAY_WIDTH) / scale);
    return width > 0 ? width : 1;
}

static uint32_t output_logical_height(struct wd_server* server) {
    double scale = server ? server->output_scale : 1.0;
    if (scale <= 0.0)
    {
        scale = 1.0;
    }

    uint32_t height = (uint32_t)((double)(server ? server->display_height : WD_DISPLAY_HEIGHT) / scale);
    return height > 0 ? height : 1;
}

static uint32_t view_surface_width_or(struct wd_view* view, uint32_t fallback) {
    struct wlr_surface* surface = view_root_surface(view);
    if (!surface)
    {
        return fallback;
    }

    int width = surface->current.width;
    if (width <= 0)
    {
        return fallback;
    }

    return (uint32_t)width;
}

static uint32_t view_surface_height_or(struct wd_view* view, uint32_t fallback) {
    struct wlr_surface* surface = view_root_surface(view);
    if (!surface)
    {
        return fallback;
    }

    int height = surface->current.height;
    if (height <= 0)
    {
        return fallback;
    }

    return (uint32_t)height;
}

static struct wd_view* view_from_xdg_toplevel(struct wd_server* server, struct wlr_xdg_toplevel* toplevel) {
    if (!server || !toplevel)
    {
        return NULL;
    }

    struct wd_view* candidate = NULL;

    wl_list_for_each(candidate, &server->views, link) {
        if (candidate->xdg_surface && candidate->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
            candidate->xdg_surface->toplevel == toplevel)
        {
            return candidate;
        }
    }

    return NULL;
}

struct wd_view* wd_scene_view_from_xdg_toplevel_resource(struct wd_server* server, struct wl_resource* toplevel_resource) {
    if (!server || !toplevel_resource)
    {
        return NULL;
    }

    struct wd_view* candidate = NULL;

    wl_list_for_each(candidate, &server->views, link) {
        if (!candidate->xdg_surface || candidate->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || !candidate->xdg_surface->toplevel)
        {
            continue;
        }

        if (candidate->xdg_surface->toplevel->resource == toplevel_resource)
        {
            return candidate;
        }
    }

    return NULL;
}

static struct wd_view* view_current_parent(struct wd_view* view) {
    if (!view || !view->server || !view->xdg_surface || view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
        !view->xdg_surface->toplevel || !view->xdg_surface->toplevel->parent)
    {
        return NULL;
    }

    return view_from_xdg_toplevel(view->server, view->xdg_surface->toplevel->parent);
}

static bool view_is_transient(struct wd_view* view) {
    return view_current_parent(view) != NULL;
}

static void view_pick_initial_size(struct wd_view* view, uint32_t* out_width, uint32_t* out_height) {
    uint32_t output_w = output_logical_width(view ? view->server : NULL);
    uint32_t output_h = output_logical_height(view ? view->server : NULL);
    uint32_t width    = output_w;
    uint32_t height   = output_h;

    if (view_is_transient(view) || (view && view->is_dialog))
    {
        struct wd_view* parent   = view_current_parent(view);
        uint32_t        parent_w = view_surface_width_or(parent, output_w);
        uint32_t        parent_h = view_surface_height_or(parent, output_h);

        /*
         * Dialogs should not be configured as full-screen-sized toplevels.
         * Give them a generous parent-relative maximum while still letting clients
         * commit smaller natural sizes.
         */
        width  = parent_w > 160 ? parent_w - 80 : parent_w;
        height = parent_h > 160 ? parent_h - 80 : parent_h;

        if (output_w > 160 && width > output_w - 160)
        {
            width = output_w - 160;
        }

        if (output_h > 120 && height > output_h - 120)
        {
            height = output_h - 120;
        }

        if (width < 320)
        {
            width = 320;
        }

        if (height < 200)
        {
            height = 200;
        }
    }
    else if (view && (view->x != 0 || view->y != 0))
    {
        width  = output_w > 160 ? output_w - 160 : output_w;
        height = output_h > 120 ? output_h - 120 : output_h;
    }

    if (out_width)
    {
        *out_width = width;
    }

    if (out_height)
    {
        *out_height = height;
    }
}

static void view_place_cascaded(struct wd_view* view) {
    if (!view || !view->server)
    {
        return;
    }

    int offset       = (int)((view->server->next_view_offset++ % 8) * 40);
    view->x          = offset;
    view->y          = offset;
    view->positioned = true;
}

static void view_place_relative_to_parent(struct wd_view* view, struct wd_view* parent) {
    if (!view || !parent)
    {
        return;
    }

    uint32_t output_w = output_logical_width(view->server);
    uint32_t output_h = output_logical_height(view->server);
    uint32_t parent_w = view_surface_width_or(parent, output_w);
    uint32_t parent_h = view_surface_height_or(parent, output_h);
    uint32_t child_w  = view_surface_width_or(view, 480);
    uint32_t child_h  = view_surface_height_or(view, 320);

    int x = parent->x + ((int)parent_w - (int)child_w) / 2;
    int y = parent->y + ((int)parent_h - (int)child_h) / 3;

    view->x = clamp_i(x, 0, (int)output_w - 1);
    view->y = clamp_i(y, 0, (int)output_h - 1);

    if (view->x + (int)child_w > (int)output_w)
    {
        view->x = clamp_i((int)output_w - (int)child_w, 0, (int)output_w - 1);
    }

    if (view->y + (int)child_h > (int)output_h)
    {
        view->y = clamp_i((int)output_h - (int)child_h, 0, (int)output_h - 1);
    }

    view->parent     = parent;
    view->positioned = true;
}

static void view_update_parent_and_position(struct wd_view* view, bool force_reposition) {
    if (!view || !view->server)
    {
        return;
    }

    int old_x = view->x;
    int old_y = view->y;

    struct wd_view* parent = view_current_parent(view);
    view->parent           = parent;

    if (parent)
    {
        if (force_reposition || !view->positioned)
        {
            view_place_relative_to_parent(view, parent);
            wd_scene_set_view_position(view);
        }

        if (view->mapped && (view->x != old_x || view->y != old_y))
        {
            wd_server_mark_view_move_dirty(view, old_x, old_y);
        }
        return;
    }

    if (!view->positioned)
    {
        view_place_cascaded(view);
        wd_scene_set_view_position(view);

        if (view->mapped && (view->x != old_x || view->y != old_y))
        {
            wd_server_mark_view_move_dirty(view, old_x, old_y);
        }
    }
}

static struct wd_view* view_topmost_modal_child(struct wd_view* parent) {
    if (!parent || !parent->server)
    {
        return NULL;
    }

    struct wd_view* child  = NULL;
    struct wd_view* result = NULL;

    wl_list_for_each(child, &parent->server->views, link) {
        if (child->parent == parent && child->mapped && child->dialog_modal)
        {
            result = child;
        }
    }

    return result;
}

static struct wd_view* view_focus_target(struct wd_view* view) {
    for (unsigned depth = 0; view && depth < 8; ++depth)
    {
        struct wd_view* modal_child = view_topmost_modal_child(view);
        if (!modal_child || modal_child == view)
        {
            break;
        }

        view = modal_child;
    }

    return view;
}

static void view_raise_with_parent(struct wd_view* view) {
    if (!view)
    {
        return;
    }

    if (view->parent && view->parent->mapped && view->parent != view)
    {
        wd_scene_raise_view(view->parent);
    }

    wd_scene_raise_view(view);
}

void wd_scene_note_dialog_state(struct wd_view* view) {
    if (!view || !view->server)
    {
        return;
    }

    /*
     * xdg-dialog is only meaningful for toplevels with a parent. If the parent
     * has already been set, refresh parent-relative placement. If the parent is
     * set later, the existing set_parent listener will do the same.
     */
    view_update_parent_and_position(view, true);

    WD_LOG_DEBUG("WayDisplay: xdg-dialog view=%p app_id=%s title=%s modal=%d parent=%p", (void*)view, view_app_id(view), view_title(view),
                 view->dialog_modal ? 1 : 0, (void*)view->parent);

    if (view->mapped)
    {
        view_raise_with_parent(view);
        if (view->dialog_modal && view->parent && view->server->focused_view == view->parent)
        {
            wd_scene_focus_view(view);
        }
    }

    wd_server_mark_view_dirty(view);
}

void wd_scene_focus_view(struct wd_view* view) {
    if (!view || !view->mapped)
    {
        return;
    }

    view = view_focus_target(view);

    struct wlr_surface* surface = view_root_surface(view);
    if (!view || !view->mapped || !surface)
    {
        return;
    }

    struct wd_server* server = view->server;

    if (server->focused_view && server->focused_view != view)
    {
        view_set_activated(server->focused_view, false);
    }

    view->minimized         = false;
    server->focused_view    = view;
    server->focused_surface = surface;

    view_set_activated(view, true);
    wd_keyboard_shortcuts_inhibit_refresh(server);

    view_raise_with_parent(view);

    wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    if (server->keyboard)
    {
        wd_keyboard_notify_enter(server, server->focused_surface);
    }

    wd_server_mark_scene_dirty(server);
}

static void surface_commit_tracker_destroy(struct wd_surface_commit_tracker* state) {
    if (!state)
    {
        return;
    }

    if (state->link.prev && state->link.next)
    {
        wl_list_remove(&state->link);
        wl_list_init(&state->link);
    }

    remove_listener_if_linked(&state->commit);
    remove_listener_if_linked(&state->destroy);
    remove_listener_if_linked(&state->view_destroy);

    free(state);
}

static void view_surface_commit_trackers_destroy(struct wd_view* view) {
    if (!view || !view->surface_commit_trackers.prev || !view->surface_commit_trackers.next)
    {
        return;
    }

    struct wd_surface_commit_tracker* state;
    struct wd_surface_commit_tracker* tmp;
    wl_list_for_each_safe(state, tmp, &view->surface_commit_trackers, link) {
        surface_commit_tracker_destroy(state);
    }
}

static struct wd_surface_commit_tracker* surface_commit_tracker_for_surface(struct wd_view* view, struct wlr_surface* surface) {
    if (!view || !surface || !view->surface_commit_trackers.prev || !view->surface_commit_trackers.next)
    {
        return NULL;
    }

    struct wd_surface_commit_tracker* state;
    wl_list_for_each(state, &view->surface_commit_trackers, link) {
        if (state->surface == surface)
        {
            return state;
        }
    }

    return NULL;
}

static void surface_commit_tracker_handle_commit(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_surface_commit_tracker* state = wl_container_of(listener, state, commit);
    if (!state || !state->view)
    {
        return;
    }

    wd_server_mark_view_dirty(state->view);

    /*
     * Child/subsurface trees can appear after the toplevel has already been
     * created. Refresh the tracked set from any commit we do observe so the
     * next child commit wakes the streamer directly instead of waiting for an
     * unrelated input event.
     */
    view_track_surface_commits(state->view);
}

static void surface_commit_tracker_handle_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_surface_commit_tracker* state = wl_container_of(listener, state, destroy);
    surface_commit_tracker_destroy(state);
}

static void surface_commit_tracker_handle_view_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_surface_commit_tracker* state = wl_container_of(listener, state, view_destroy);
    surface_commit_tracker_destroy(state);
}

static void track_surface_commit_iterator(struct wlr_surface* surface, int sx, int sy, void* data) {
    (void)sx;
    (void)sy;

    struct wd_view* view = data;
    if (!view || !surface || surface_commit_tracker_for_surface(view, surface))
    {
        return;
    }

    struct wd_surface_commit_tracker* state = calloc(1, sizeof(*state));
    if (!state)
    {
        WD_LOG_ERROR("WayDisplay: failed to allocate xdg surface commit tracker");
        return;
    }

    wl_list_init(&state->link);
    wl_list_init(&state->commit.link);
    wl_list_init(&state->destroy.link);
    wl_list_init(&state->view_destroy.link);

    state->surface = surface;
    state->view    = view;

    state->commit.notify = surface_commit_tracker_handle_commit;
    wl_signal_add(&surface->events.commit, &state->commit);

    state->destroy.notify = surface_commit_tracker_handle_destroy;
    wl_signal_add(&surface->events.destroy, &state->destroy);

    if (view->xdg_surface)
    {
        state->view_destroy.notify = surface_commit_tracker_handle_view_destroy;
        wl_signal_add(&view->xdg_surface->events.destroy, &state->view_destroy);
    }

    wl_list_insert(&view->surface_commit_trackers, &state->link);
}

static void view_track_surface_commits(struct wd_view* view) {
    if (!view || !view->xdg_surface || !view->surface_commit_trackers.prev || !view->surface_commit_trackers.next)
    {
        return;
    }

    wlr_xdg_surface_for_each_surface(view->xdg_surface, track_surface_commit_iterator, view);
}

static int32_t preferred_buffer_scale_for(double scale) {
    if (scale <= 1.0)
    {
        return 1;
    }

    return (int32_t)(scale + 0.999999);
}

static void surface_apply_fractional_scale(struct wlr_surface* surface, int sx, int sy, void* data) {
    (void)sx;
    (void)sy;

    struct wd_server* server = data;
    if (!surface || !server)
    {
        return;
    }

    double scale = server->output_scale;
    if (scale <= 0.0)
    {
        scale = 1.0;
    }

    /*
     * wp_fractional_scale_v1 tells clients the preferred fractional scale.
     * wl_surface preferred buffer scale remains integer, so use ceil(scale)
     * there to keep clients from allocating too-small buffers.
     */
    wlr_fractional_scale_v1_notify_scale(surface, scale);
    wlr_surface_set_preferred_buffer_scale(surface, preferred_buffer_scale_for(scale));
}

static void view_apply_fractional_scale(struct wd_view* view) {
    if (!view || !view->server || !view->xdg_surface)
    {
        return;
    }

    wlr_xdg_surface_for_each_surface(view->xdg_surface, surface_apply_fractional_scale, view->server);
}

static void view_schedule_initial_configure(struct wd_view* view) {
    if (!view || view->configured_once || view->configure_idle || !view->server || !view->server->event_loop)
    {
        return;
    }

    view->configure_idle = wl_event_loop_add_idle(view->server->event_loop, view_configure_idle, view);
}

static void view_configure_idle(void* data) {
    struct wd_view* view = data;

    if (!view)
    {
        return;
    }

    view->configure_idle = NULL;

    if (view->configured_once || !xdg_toplevel_can_configure(view))
    {
        return;
    }

    /*
     * wlroots 0.19: defer initial configure until idle after initialization.
     */
    uint32_t width  = 0;
    uint32_t height = 0;
    view_pick_initial_size(view, &width, &height);

    view_set_bounds(view, width, height);
    wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, width, height);

    /*
     * Initial configure should not imply focus. Activate the view when it maps
     * and actually becomes the focused surface.
     */
    wlr_xdg_toplevel_set_wm_capabilities(view->xdg_surface->toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                                                                          WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
                                                                          WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);

    view_apply_fractional_scale(view);

    view->configured_once = true;

    WD_LOG_DEBUG("WayDisplay: sent initial xdg toplevel configure");
}

void wd_scene_raise_view(struct wd_view* view) {
    if (!view || !view->scene_tree)
    {
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

    wd_server_mark_view_dirty(view);
}

static void view_handle_commit(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, commit);

    wd_server_mark_view_dirty(view);
    view_track_surface_commits(view);
    view_apply_fractional_scale(view);

    if (!view->configured_once && xdg_toplevel_can_configure(view))
    {
        view_schedule_initial_configure(view);
    }
}

static void view_handle_set_parent(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, set_parent);

    if (!view)
    {
        return;
    }

    view_update_parent_and_position(view, true);
    wd_server_mark_view_dirty(view);
}

static void view_handle_map(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, map);

    view_update_parent_and_position(view, false);
    view_update_metadata(view);

    view->mapped    = true;
    view->minimized = false;

    wd_scene_set_view_position(view);
    view_track_surface_commits(view);
    view_apply_fractional_scale(view);
    wd_scene_focus_view(view);
    wd_server_mark_view_dirty(view);

    WD_LOG_DEBUG("WayDisplay: xdg toplevel mapped scene_tree=%p", (void*)view->scene_tree);
}

static void view_handle_unmap(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, unmap);

    struct wd_server* server = view->server;

    view->mapped = false;
    view_set_activated(view, false);

    server_clear_focus_if_view(server, view, true);

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

    wd_server_mark_scene_dirty(server);
}

static void view_handle_xdg_toplevel_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xdg_toplevel_destroy);

    /*
     * request_move belongs to wlr_xdg_toplevel and must be gone before
     * wlroots destroys/checks the toplevel listener lists.
     *
     * Do NOT free view here. Surface map/unmap/commit/destroy listeners may
     * still exist and may still fire.
     */
    remove_listener_if_linked(&view->request_move);
    remove_listener_if_linked(&view->set_parent);
    remove_listener_if_linked(&view->set_app_id);
    remove_listener_if_linked(&view->set_title);
    remove_listener_if_linked(&view->request_resize);
    remove_listener_if_linked(&view->request_maximize);
    remove_listener_if_linked(&view->request_fullscreen);
    remove_listener_if_linked(&view->request_minimize);
    remove_listener_if_linked(&view->xdg_toplevel_destroy);
}

static void view_handle_xdg_surface_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, xdg_surface_destroy);

    struct wd_server* server = view->server;

    remove_listener_if_linked(&view->request_move);
    remove_listener_if_linked(&view->set_parent);
    remove_listener_if_linked(&view->set_app_id);
    remove_listener_if_linked(&view->set_title);
    remove_listener_if_linked(&view->request_resize);
    remove_listener_if_linked(&view->request_maximize);
    remove_listener_if_linked(&view->request_fullscreen);
    remove_listener_if_linked(&view->request_minimize);
    remove_listener_if_linked(&view->xdg_toplevel_destroy);

    view_surface_commit_trackers_destroy(view);

    remove_listener_if_linked(&view->new_popup);
    remove_listener_if_linked(&view->map);
    remove_listener_if_linked(&view->unmap);
    remove_listener_if_linked(&view->commit);
    remove_listener_if_linked(&view->xdg_surface_destroy);

    if (view->link.prev && view->link.next)
    {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    struct wd_view* child = NULL;
    wl_list_for_each(child, &server->views, link) {
        if (child->parent == view)
        {
            child->parent = NULL;
            if (!child->positioned)
            {
                view_update_parent_and_position(child, false);
            }
        }
    }

    if (view->configure_idle)
    {
        wl_event_source_remove(view->configure_idle);
        view->configure_idle = NULL;
    }

    server_clear_focus_if_view(server, view, true);

    if (view->xdg_surface && server->focused_surface == view->xdg_surface->surface)
    {
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

    if (view->toplevel_icon)
    {
        wlr_xdg_toplevel_icon_v1_unref(view->toplevel_icon);
        view->toplevel_icon = NULL;
    }

    free(view->app_id);
    free(view->title);

    if (view->xdg_dialog_resource)
    {
        wl_resource_set_user_data(view->xdg_dialog_resource, NULL);
        view->xdg_dialog_resource = NULL;
    }

    if (view->scene_tree)
    {
        view->scene_tree->node.data = NULL;
        view->scene_tree            = NULL;
    }

    wd_server_mark_scene_dirty(server);

    free(view);
}

static void popup_unconstrain_destroy(struct wd_popup_unconstrain* state) {
    if (!state)
    {
        return;
    }

    remove_listener_if_linked(&state->popup_destroy);
    remove_listener_if_linked(&state->popup_surface_destroy);
    remove_listener_if_linked(&state->view_destroy);

    if (state->idle)
    {
        wl_event_source_remove(state->idle);
        state->idle = NULL;
    }

    free(state);
}

static void popup_unconstrain_handle_popup_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_unconstrain* state = wl_container_of(listener, state, popup_destroy);

    popup_unconstrain_destroy(state);
}

static void popup_unconstrain_handle_popup_surface_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_unconstrain* state = wl_container_of(listener, state, popup_surface_destroy);

    popup_unconstrain_destroy(state);
}

static void popup_unconstrain_handle_view_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_unconstrain* state = wl_container_of(listener, state, view_destroy);

    popup_unconstrain_destroy(state);
}

static void popup_unconstrain_idle(void* data) {
    struct wd_popup_unconstrain* state = data;

    if (!state)
    {
        return;
    }

    state->idle = NULL;

    if (!state->popup || !state->popup->base || !xdg_surface_can_configure(state->popup->base) || !state->view || !state->view->server ||
        !state->view->xdg_surface || !state->view->xdg_surface->surface)
    {
        popup_unconstrain_destroy(state);
        return;
    }

    /*
     * This callback runs after the immediate xdg_popup/new_popup signal returns.
     * That avoids calling wlr_xdg_popup_unconstrain_from_box() while wlroots is
     * still in the middle of xdg_surface initialization, which can assert in
     * wlr_xdg_surface_schedule_configure().
     *
     * The constraint box is in root/toplevel-surface coordinates, not global
     * layout coordinates.
     */
    int root_width  = state->view->xdg_surface->surface->current.width;
    int root_height = state->view->xdg_surface->surface->current.height;

    if (root_width <= 0)
    {
        root_width = (int)state->view->server->display_width;
    }

    if (root_height <= 0)
    {
        root_height = (int)state->view->server->display_height;
    }

    struct wlr_box root_box = {
        .x      = 0,
        .y      = 0,
        .width  = root_width,
        .height = root_height,
    };

    wlr_xdg_popup_unconstrain_from_box(state->popup, &root_box);
    wd_server_mark_view_dirty(state->view);

    popup_unconstrain_destroy(state);
}

static void view_schedule_popup_unconstrain(struct wd_view* view, struct wlr_xdg_popup* popup) {
    if (!view || !view->server || !view->server->event_loop || !popup || !popup->base)
    {
        return;
    }

    struct wd_popup_unconstrain* state = calloc(1, sizeof(*state));
    if (!state)
    {
        WD_LOG_ERROR("WayDisplay: failed to allocate popup unconstrain state");
        return;
    }

    state->popup = popup;
    state->view  = view;
    wl_list_init(&state->popup_destroy.link);
    wl_list_init(&state->popup_surface_destroy.link);
    wl_list_init(&state->view_destroy.link);

    state->popup_destroy.notify = popup_unconstrain_handle_popup_destroy;
    wl_signal_add(&popup->base->events.destroy, &state->popup_destroy);

    if (popup->base->surface)
    {
        state->popup_surface_destroy.notify = popup_unconstrain_handle_popup_surface_destroy;
        wl_signal_add(&popup->base->surface->events.destroy, &state->popup_surface_destroy);
    }

    if (view->xdg_surface)
    {
        state->view_destroy.notify = popup_unconstrain_handle_view_destroy;
        wl_signal_add(&view->xdg_surface->events.destroy, &state->view_destroy);
    }

    state->idle = wl_event_loop_add_idle(view->server->event_loop, popup_unconstrain_idle, state);
    if (!state->idle)
    {
        popup_unconstrain_destroy(state);
        return;
    }
}

static void popup_commit_tracker_destroy(struct wd_popup_commit_tracker* state) {
    if (!state)
    {
        return;
    }

    if (state->link.prev && state->link.next)
    {
        wl_list_remove(&state->link);
        wl_list_init(&state->link);
    }

    remove_listener_if_linked(&state->commit);
    remove_listener_if_linked(&state->map);
    remove_listener_if_linked(&state->unmap);
    remove_listener_if_linked(&state->destroy);
    remove_listener_if_linked(&state->view_destroy);

    if (state->scene_tree)
    {
        state->scene_tree->node.data = NULL;
        state->scene_tree            = NULL;
    }

    free(state);
}

static struct wd_popup_commit_tracker* popup_commit_tracker_for_popup(struct wd_server* server, struct wlr_xdg_popup* popup) {
    if (!server || !popup)
    {
        return NULL;
    }

    if (!server->popup_commit_trackers.prev || !server->popup_commit_trackers.next)
    {
        return NULL;
    }

    struct wd_popup_commit_tracker* state;
    wl_list_for_each(state, &server->popup_commit_trackers, link) {
        if (state->popup == popup)
        {
            return state;
        }
    }

    return NULL;
}

static struct wd_popup_commit_tracker* popup_commit_tracker_for_surface(struct wd_server* server, struct wlr_surface* surface) {
    if (!server || !surface)
    {
        return NULL;
    }

    if (!server->popup_commit_trackers.prev || !server->popup_commit_trackers.next)
    {
        return NULL;
    }

    struct wd_popup_commit_tracker* state;
    wl_list_for_each(state, &server->popup_commit_trackers, link) {
        if (state->popup && state->popup->base && state->popup->base->surface == surface)
        {
            return state;
        }
    }

    return NULL;
}

static bool popup_commit_tracker_ensure_scene_tree(struct wd_popup_commit_tracker* state) {
    if (!state || state->scene_tree)
    {
        return state && state->scene_tree;
    }

    struct wd_view*      view   = state->view;
    struct wlr_xdg_popup* popup = state->popup;
    if (!view || !view->server || !view->scene_tree || !popup || !popup->base)
    {
        return false;
    }

    struct wlr_scene_tree*          parent_tree  = view->scene_tree;
    struct wd_popup_commit_tracker* parent_popup = NULL;
    if (popup->parent)
    {
        parent_popup = popup_commit_tracker_for_surface(view->server, popup->parent);
        if (parent_popup && parent_popup->scene_tree)
        {
            parent_tree = parent_popup->scene_tree;
        }
    }

    if (!parent_tree)
    {
        WD_LOG_DEBUG("WayDisplay: not creating explicit popup scene tree popup=%p view=%p because "
                     "parent scene tree is NULL parent_popup=%p parent_surface=%p",
                     (void*)popup, (void*)view, parent_popup ? (void*)parent_popup->popup : NULL,
                     popup->parent ? (void*)popup->parent : NULL);
        return false;
    }

    state->scene_tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    if (!state->scene_tree)
    {
        view->server->net.stats.popup_explicit_scene_tree_failures++;
        WD_LOG_ERROR("WayDisplay: failed to create explicit xdg popup scene tree "
                     "popup=%p parent_view=%p parent_popup=%p parent_surface=%p",
                     (void*)popup, (void*)view, parent_popup ? (void*)parent_popup->popup : NULL,
                     popup->parent ? (void*)popup->parent : NULL);
        return false;
    }

    view->server->net.stats.popup_explicit_scene_trees++;
    state->scene_tree->node.data = view;
    wlr_scene_node_set_position(&state->scene_tree->node, popup->current.geometry.x, popup->current.geometry.y);
    wlr_scene_node_raise_to_top(&state->scene_tree->node);
    WD_LOG_DEBUG("WayDisplay: created explicit xdg popup scene tree popup=%p "
                 "scene_tree=%p parent_view=%p parent_popup=%p parent_surface=%p "
                 "parent_tree=%p",
                 (void*)popup, (void*)state->scene_tree, (void*)view, parent_popup ? (void*)parent_popup->popup : NULL,
                 popup->parent ? (void*)popup->parent : NULL, (void*)parent_tree);
    return true;
}

static void popup_commit_tracker_mark_dirty(struct wd_popup_commit_tracker* state, const char* reason) {
    if (!state || !state->view || !state->view->server || !state->popup || !state->popup->base)
    {
        return;
    }

    struct wlr_surface* surface        = state->popup->base->surface;
    int                 surface_width  = surface ? surface->current.width : 0;
    int                 surface_height = surface ? surface->current.height : 0;

    int geometry_x      = state->popup->current.geometry.x;
    int geometry_y      = state->popup->current.geometry.y;
    int geometry_width  = state->popup->current.geometry.width;
    int geometry_height = state->popup->current.geometry.height;

    if (state->scene_tree)
    {
        wlr_scene_node_set_position(&state->scene_tree->node, geometry_x, geometry_y);
        wlr_scene_node_raise_to_top(&state->scene_tree->node);
    }

    int geometry_changed = !state->have_last_geometry || state->last_surface_width != surface_width ||
                           state->last_surface_height != surface_height || state->last_geometry_x != geometry_x ||
                           state->last_geometry_y != geometry_y || state->last_geometry_width != geometry_width ||
                           state->last_geometry_height != geometry_height;

    if (geometry_changed)
    {
        WD_LOG_DEBUG("WayDisplay: xdg popup %s popup=%p base=%p parent=%p "
                     "surface=%dx%d geom=%d,%d %dx%d",
                     reason ? reason : "changed", (void*)state->popup, (void*)state->popup->base,
                     state->popup->parent ? (void*)state->popup->parent : NULL, surface_width, surface_height, geometry_x, geometry_y,
                     geometry_width, geometry_height);

        state->have_last_geometry   = 1;
        state->last_surface_width   = surface_width;
        state->last_surface_height  = surface_height;
        state->last_geometry_x      = geometry_x;
        state->last_geometry_y      = geometry_y;
        state->last_geometry_width  = geometry_width;
        state->last_geometry_height = geometry_height;
    }

    /*
     * Popup commits are not associated with a wd_view commit listener.  Without
     * explicitly marking the scene dirty, WayDisplay may render once while the
     * popup still has 0x0 geometry and then never stream the later real popup
     * content.
     *
     * Do not re-run wlr_xdg_popup_unconstrain_from_box() on every popup commit.
     * Some toolkits create and destroy nested popup surfaces quickly while menus
     * are open; repeatedly scheduling popup configures from commit/map/unmap can
     * race with wlroots popup teardown and crash in
     * wlr_xdg_surface_schedule_configure().  The popup is unconstrained once when
     * it is first observed instead.
     */
    wd_server_mark_view_dirty(state->view);
}

static void popup_commit_tracker_handle_commit(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_commit_tracker* state = wl_container_of(listener, state, commit);

    popup_commit_tracker_mark_dirty(state, "commit");
}

static void popup_commit_tracker_handle_map(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_commit_tracker* state = wl_container_of(listener, state, map);

    popup_commit_tracker_mark_dirty(state, "map");

    if (state->view && state->view->server)
    {
        wd_pointer_clear_focus(state->view->server);
    }
}

static void popup_commit_tracker_handle_unmap(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_commit_tracker* state = wl_container_of(listener, state, unmap);

    popup_commit_tracker_mark_dirty(state, "unmap");
}

static void popup_commit_tracker_handle_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_commit_tracker* state = wl_container_of(listener, state, destroy);

    if (state->view && state->view->server)
    {
        wd_server_mark_view_dirty(state->view);
    }

    popup_commit_tracker_destroy(state);
}

static void popup_commit_tracker_handle_view_destroy(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_popup_commit_tracker* state = wl_container_of(listener, state, view_destroy);

    popup_commit_tracker_destroy(state);
}

static void view_track_popup_commits(struct wd_view* view, struct wlr_xdg_popup* popup) {
    if (view && popup_commit_tracker_for_popup(view->server, popup))
    {
        WD_LOG_DEBUG("WayDisplay: xdg popup already tracked popup=%p view=%p", (void*)popup, (void*)view);
        return;
    }

    if (!view)
    {
        WD_LOG_DEBUG("WayDisplay: not tracking xdg popup because view is NULL popup=%p", (void*)popup);
        return;
    }

    if (!popup)
    {
        WD_LOG_DEBUG("WayDisplay: not tracking xdg popup for view=%p because popup is NULL", (void*)view);
        return;
    }

    if (!popup->base)
    {
        WD_LOG_DEBUG("WayDisplay: not tracking xdg popup popup=%p for view=%p because base is NULL", (void*)popup, (void*)view);
        return;
    }

    if (!popup->base->surface)
    {
        WD_LOG_DEBUG("WayDisplay: not tracking xdg popup popup=%p base=%p for view=%p because base "
                     "surface is NULL",
                     (void*)popup, (void*)popup->base, (void*)view);
        return;
    }

    struct wd_popup_commit_tracker* state = calloc(1, sizeof(*state));
    if (!state)
    {
        WD_LOG_ERROR("WayDisplay: failed to allocate popup commit tracker");
        return;
    }

    wl_list_init(&state->link);
    state->popup = popup;
    state->view  = view;

    /*
     * Normal per-toplevel popups are usually owned by the toplevel scene tree.
     * Shell-level/global popups from some toolkits, notably Qt/KDE context
     * menus, can arrive only through xdg_shell.events.new_popup on wlroots
     * versions this compositor targets.  Those are upgraded below by the global
     * popup handler with an explicit scene tree so they are visible without
     * reverting the patch-26 cleanup for every popup.
     */
    wl_list_insert(&view->server->popup_commit_trackers, &state->link);

    wl_list_init(&state->commit.link);
    wl_list_init(&state->map.link);
    wl_list_init(&state->unmap.link);
    wl_list_init(&state->destroy.link);
    wl_list_init(&state->view_destroy.link);

    state->commit.notify = popup_commit_tracker_handle_commit;
    wl_signal_add(&popup->base->surface->events.commit, &state->commit);

    state->map.notify = popup_commit_tracker_handle_map;
    wl_signal_add(&popup->base->surface->events.map, &state->map);

    state->unmap.notify = popup_commit_tracker_handle_unmap;
    wl_signal_add(&popup->base->surface->events.unmap, &state->unmap);

    state->destroy.notify = popup_commit_tracker_handle_destroy;
    wl_signal_add(&popup->base->events.destroy, &state->destroy);

    if (view->xdg_surface)
    {
        state->view_destroy.notify = popup_commit_tracker_handle_view_destroy;
        wl_signal_add(&view->xdg_surface->events.destroy, &state->view_destroy);
    }
}

static void view_handle_new_popup(struct wl_listener* listener, void* data) {
    struct wd_view* view = wl_container_of(listener, view, new_popup);

    struct wlr_xdg_popup* popup = data;

    if (!view || !popup)
    {
        return;
    }

    /*
     * Defer unconstraining. Calling wlr_xdg_popup_unconstrain_from_box()
     * directly from new_popup can schedule a configure before the popup
     * xdg_surface is fully initialized.
     */
    if (!popup_commit_tracker_for_popup(view->server, popup))
    {
        view_schedule_popup_unconstrain(view, popup);
    }
    view_track_popup_commits(view, popup);

    /*
     * wlr_scene_xdg_surface_create() should already create scene nodes for
     * the xdg surface tree. We do not create a wd_view for popups.
     *
     * The important part is that we acknowledge/configure popup creation and
     * mark the scene dirty so it gets rendered.
     */
    WD_LOG_DEBUG("WayDisplay: new xdg popup for view=%p popup=%p base=%p "
                 "parent=%p geom=%d,%d %dx%d",
                 (void*)view, (void*)popup, popup && popup->base ? (void*)popup->base : NULL,
                 popup && popup->parent ? (void*)popup->parent : NULL, popup ? popup->current.geometry.x : 0,
                 popup ? popup->current.geometry.y : 0, popup ? popup->current.geometry.width : 0,
                 popup ? popup->current.geometry.height : 0);

    view_track_surface_commits(view);
    view_apply_fractional_scale(view);
    wd_server_mark_view_dirty(view);
}

static void view_handle_set_app_id(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, set_app_id);
    view_update_app_id(view);

    WD_LOG_DEBUG("WayDisplay: toplevel metadata app_id=%s title=%s", view_app_id(view), view_title(view));
}

static void view_handle_set_title(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, set_title);
    view_update_title(view);

    WD_LOG_DEBUG("WayDisplay: toplevel metadata app_id=%s title=%s", view_app_id(view), view_title(view));
}

static bool view_request_has_valid_pointer_grab(struct wd_view* view, struct wlr_seat_client* seat_client, uint32_t serial) {
    if (!view || !view->server || !view->server->seat || !seat_client || !view->mapped)
    {
        return false;
    }

    if (seat_client->seat != view->server->seat)
    {
        return false;
    }

    struct wlr_surface* surface = view_root_surface(view);
    if (!surface)
    {
        return false;
    }

    return wlr_seat_validate_pointer_grab_serial(view->server->seat, surface, serial);
}

static void view_handle_request_move(struct wl_listener* listener, void* data) {
    struct wd_view*                     view  = wl_container_of(listener, view, request_move);
    struct wlr_xdg_toplevel_move_event* event = data;

    if (!view || !view->mapped || !event)
    {
        return;
    }

    if (view->fullscreen)
    {
        WD_LOG_DEBUG("WayDisplay: ignoring xdg_toplevel.request_move while fullscreen");
        return;
    }

    if (!view_request_has_valid_pointer_grab(view, event->seat, event->serial))
    {
        WD_LOG_DEBUG("WayDisplay: ignoring xdg_toplevel.request_move with invalid pointer grab serial=%u", event->serial);
        view->server->net.stats.xdg_move_invalid_serial++;
        return;
    }

    wd_scene_focus_view(view);
    wd_pointer_begin_move(view->server, view);
}

static void view_handle_request_resize(struct wl_listener* listener, void* data) {
    struct wd_view*                       view  = wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event* event = data;

    if (!view || !view->mapped || !event || event->edges == WLR_EDGE_NONE)
    {
        return;
    }

    if (view->fullscreen)
    {
        WD_LOG_DEBUG("WayDisplay: ignoring xdg_toplevel.request_resize while fullscreen");
        return;
    }

    if (!view_request_has_valid_pointer_grab(view, event->seat, event->serial))
    {
        WD_LOG_DEBUG("WayDisplay: ignoring xdg_toplevel.request_resize with invalid pointer grab serial=%u edges=%u", event->serial, event->edges);
        view->server->net.stats.xdg_resize_invalid_serial++;
        return;
    }

    wd_scene_focus_view(view);
    wd_pointer_begin_resize(view->server, view, event->edges);
}

static void view_restore_saved_geometry(struct wd_view* view) {
    if (!view || !view->xdg_surface || !view->xdg_surface->toplevel)
    {
        return;
    }

    view_mark_geometry_before_change(view);

    view->x = view->saved_x;
    view->y = view->saved_y;

    wd_scene_set_view_position(view);
    if (xdg_toplevel_can_configure(view))
    {
        wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, view->saved_width, view->saved_height);
    }

    view_mark_geometry_after_change(view);
}

static void view_save_geometry(struct wd_view* view) {
    if (!view || !view->xdg_surface || !view->xdg_surface->surface)
    {
        return;
    }

    view->saved_x = view->x;
    view->saved_y = view->y;

    int width  = view->xdg_surface->surface->current.width;
    int height = view->xdg_surface->surface->current.height;

    if (width <= 0)
    {
        width = (int)view->server->display_width;
    }

    if (height <= 0)
    {
        height = (int)view->server->display_height;
    }

    view->saved_width  = (uint32_t)width;
    view->saved_height = (uint32_t)height;
}

static void view_handle_request_maximize(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, request_maximize);

    if (!view || !view->xdg_surface || !view->xdg_surface->toplevel)
    {
        return;
    }

    bool maximize = view->xdg_surface->toplevel->requested.maximized;

    if (maximize && !view->maximized)
    {
        view_mark_geometry_before_change(view);
        view_save_geometry(view);

        view->x = 0;
        view->y = 0;
        wd_scene_set_view_position(view);

        double scale = view->server->output_scale;
        if (scale <= 0.0)
        {
            scale = 1.0;
        }

        if (xdg_toplevel_can_configure(view))
        {
            wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, (uint32_t)((double)view->server->display_width / scale),
                                      (uint32_t)((double)view->server->display_height / scale));
        }
    }
    else if (!maximize && view->maximized)
    {
        view_restore_saved_geometry(view);
    }

    view->maximized   = maximize;
    view->minimized   = false;
    view->tiled_edges = 0;
    if (xdg_toplevel_can_configure(view))
    {
        wlr_xdg_toplevel_set_maximized(view->xdg_surface->toplevel, maximize);
    }
    wd_scene_focus_view(view);
    wd_server_mark_view_dirty(view);
}

static void view_handle_request_fullscreen(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, request_fullscreen);

    if (!view || !view->xdg_surface || !view->xdg_surface->toplevel)
    {
        return;
    }

    bool fullscreen = view->xdg_surface->toplevel->requested.fullscreen;

    if (fullscreen && !view->fullscreen)
    {
        view_mark_geometry_before_change(view);
        view_save_geometry(view);
        view->x = 0;
        view->y = 0;
        wd_scene_set_view_position(view);

        double scale = view->server->output_scale;
        if (scale <= 0.0)
        {
            scale = 1.0;
        }

        if (xdg_toplevel_can_configure(view))
        {
            wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, (uint32_t)((double)view->server->display_width / scale),
                                      (uint32_t)((double)view->server->display_height / scale));
        }
    }
    else if (!fullscreen && view->fullscreen)
    {
        view_restore_saved_geometry(view);
    }

    view->fullscreen  = fullscreen;
    view->minimized   = false;
    view->tiled_edges = 0;
    if (xdg_toplevel_can_configure(view))
    {
        wlr_xdg_toplevel_set_fullscreen(view->xdg_surface->toplevel, fullscreen);
    }
    wd_scene_focus_view(view);
    wd_server_mark_view_dirty(view);
}

static void view_handle_request_minimize(struct wl_listener* listener, void* data) {
    (void)data;

    struct wd_view* view = wl_container_of(listener, view, request_minimize);

    if (!view || !view->xdg_surface || !view->xdg_surface->toplevel)
    {
        return;
    }

    /*
     * WayDisplay does not have a taskbar/window-list yet, so there is nowhere
     * useful to park a minimized window. Track the state and deactivate it, but
     * keep it mapped. Future UI can use view->minimized to actually hide/list it.
     */
    view->minimized = true;
    view_set_activated(view, false);

    server_clear_focus_if_view(view->server, view, true);

    if (xdg_surface_can_configure(view->xdg_surface))
    {
        wlr_xdg_surface_schedule_configure(view->xdg_surface);
    }
    wd_server_mark_view_dirty(view);
}

static void server_handle_new_xdg_surface(struct wl_listener* listener, void* data) {
    (void)listener;

    struct wlr_xdg_surface* xdg_surface = data;

    if (!xdg_surface)
    {
        return;
    }

    WD_LOG_DEBUG("WayDisplay: new xdg surface role=%d", xdg_surface->role);
}

static void server_handle_new_xdg_popup(struct wl_listener* listener, void* data) {
    struct wd_server*     server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup* popup  = data;

    if (!server)
    {
        WD_LOG_DEBUG("WayDisplay: ignoring global xdg popup because server is NULL popup=%p", (void*)popup);
        return;
    }

    if (!popup)
    {
        WD_LOG_DEBUG("WayDisplay: ignoring global xdg popup because popup is NULL");
        return;
    }

    /*
     * Some toolkits, including Qt/KDE apps, create context menus through the
     * shell-level new_popup event.  The per-toplevel xdg_surface new_popup
     * listener is not enough to observe those on all wlroots versions.
     *
     * For ordinary per-toplevel popup signals, the parent xdg scene tree should
     * already own rendering.  The shell-level signal is different: Qt/KDE
     * context menus can show up here without a corresponding tracked parent
     * popup scene in this compositor, so upgrade the tracker to an explicit scene
     * subtree.  Use the focused view as the popup parent anchor; right-click
     * context menus are created from the currently focused toplevel.
     */
    WD_LOG_DEBUG("WayDisplay: new global xdg popup popup=%p base=%p focused_view=%p", (void*)popup, popup->base ? (void*)popup->base : NULL,
                 (void*)server->focused_view);

    if (!popup->base)
    {
        WD_LOG_DEBUG("WayDisplay: global xdg popup attach deferred/failed popup=%p because base is "
                     "NULL focused_view=%p",
                     (void*)popup, (void*)server->focused_view);
    }
    else if (!popup->base->surface)
    {
        WD_LOG_DEBUG("WayDisplay: global xdg popup attach deferred/failed popup=%p base=%p because "
                     "base surface is NULL focused_view=%p",
                     (void*)popup, (void*)popup->base, (void*)server->focused_view);
    }
    else if (!server->focused_view)
    {
        WD_LOG_DEBUG("WayDisplay: global xdg popup attach deferred/failed popup=%p base=%p "
                     "surface=%p because focused_view is NULL",
                     (void*)popup, (void*)popup->base, (void*)popup->base->surface);
    }
    else
    {
        WD_LOG_DEBUG("WayDisplay: global xdg popup using focused view popup=%p base=%p surface=%p "
                     "view=%p scene_tree=%p parent=%p",
                     (void*)popup, (void*)popup->base, (void*)popup->base->surface, (void*)server->focused_view,
                     server->focused_view ? (void*)server->focused_view->scene_tree : NULL, popup->parent ? (void*)popup->parent : NULL);
        if (!popup_commit_tracker_for_popup(server, popup))
        {
            view_schedule_popup_unconstrain(server->focused_view, popup);
        }
        view_track_popup_commits(server->focused_view, popup);
        struct wd_popup_commit_tracker* tracker = popup_commit_tracker_for_popup(server, popup);
        if (tracker)
        {
            popup_commit_tracker_ensure_scene_tree(tracker);
        }
        view_track_surface_commits(server->focused_view);
        view_apply_fractional_scale(server->focused_view);
    }

    wd_server_mark_scene_dirty(server);
}

static void server_handle_new_xdg_toplevel(struct wl_listener* listener, void* data) {
    struct wd_server* server = wl_container_of(listener, server, new_xdg_toplevel);

    struct wlr_xdg_toplevel* toplevel = data;
    if (!server || !toplevel || !toplevel->base || !toplevel->base->surface)
    {
        return;
    }

    struct wlr_xdg_surface* xdg_surface = toplevel->base;

    struct wd_view* view = calloc(1, sizeof(*view));
    if (!view)
    {
        return;
    }

    wl_list_init(&view->link);
    wl_list_init(&view->map.link);
    wl_list_init(&view->unmap.link);
    wl_list_init(&view->commit.link);
    wl_list_init(&view->request_move.link);
    wl_list_init(&view->set_parent.link);
    wl_list_init(&view->set_app_id.link);
    wl_list_init(&view->set_title.link);
    wl_list_init(&view->request_resize.link);
    wl_list_init(&view->request_maximize.link);
    wl_list_init(&view->request_fullscreen.link);
    wl_list_init(&view->request_minimize.link);
    wl_list_init(&view->xdg_surface_destroy.link);
    wl_list_init(&view->xdg_toplevel_destroy.link);
    wl_list_init(&view->new_popup.link);
    wl_list_init(&view->surface_commit_trackers);

    view->server      = server;
    view->xdg_surface = xdg_surface;
    view_update_metadata(view);
    view_update_parent_and_position(view, false);

    view->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface);

    if (view->scene_tree)
    {
        view->scene_tree->node.data = view;
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    }

    view_track_surface_commits(view);

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
    wl_signal_add(&xdg_surface->events.destroy, &view->xdg_surface_destroy);

    /*
     * Toplevel destroy only removes toplevel-owned listeners.
     */
    view->xdg_toplevel_destroy.notify = view_handle_xdg_toplevel_destroy;
    wl_signal_add(&toplevel->events.destroy, &view->xdg_toplevel_destroy);

    view->request_move.notify = view_handle_request_move;
    wl_signal_add(&xdg_surface->toplevel->events.request_move, &view->request_move);

    view->set_parent.notify = view_handle_set_parent;
    wl_signal_add(&xdg_surface->toplevel->events.set_parent, &view->set_parent);

    view->set_app_id.notify = view_handle_set_app_id;
    wl_signal_add(&xdg_surface->toplevel->events.set_app_id, &view->set_app_id);

    view->set_title.notify = view_handle_set_title;
    wl_signal_add(&xdg_surface->toplevel->events.set_title, &view->set_title);

    view->request_resize.notify = view_handle_request_resize;
    wl_signal_add(&xdg_surface->toplevel->events.request_resize, &view->request_resize);

    view->request_maximize.notify = view_handle_request_maximize;
    wl_signal_add(&xdg_surface->toplevel->events.request_maximize, &view->request_maximize);

    view->request_fullscreen.notify = view_handle_request_fullscreen;
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &view->request_fullscreen);

    view->request_minimize.notify = view_handle_request_minimize;
    wl_signal_add(&xdg_surface->toplevel->events.request_minimize, &view->request_minimize);

    view->new_popup.notify = view_handle_new_popup;
    wl_signal_add(&xdg_surface->events.new_popup, &view->new_popup);

    WD_LOG_DEBUG("WayDisplay: new xdg toplevel scene_tree=%p", (void*)view->scene_tree);

    /*
     * Wait for the first surface commit before sending the initial configure.
     * wlroots can emit new_toplevel and metadata events before the xdg_surface
     * is initialized, and every toplevel state setter schedules a configure.
     */
}
