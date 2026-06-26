#include "wd_server_internal.h"

#include <stddef.h>

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

static struct wd_view* view_from_xdg_toplevel(struct wd_server* server, struct wlr_xdg_toplevel* toplevel) {
    if (!server || !toplevel)
    {
        return NULL;
    }

    struct wd_view* view = NULL;

    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_surface || view->xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || view->xdg_surface->toplevel != toplevel)
        {
            continue;
        }

        return view;
    }

    return NULL;
}

static size_t icon_buffer_count(struct wlr_xdg_toplevel_icon_v1* icon) {
    if (!icon)
    {
        return 0;
    }

    size_t                                  count  = 0;
    struct wlr_xdg_toplevel_icon_v1_buffer* buffer = NULL;

    wl_list_for_each(buffer, &icon->buffers, link) {
        count++;
    }

    return count;
}

static void handle_set_xdg_toplevel_icon(struct wl_listener* listener, void* data) {
    struct wd_server*                                       server = wl_container_of(listener, server, set_xdg_toplevel_icon);
    struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event* event  = data;

    if (!server || !event || !event->toplevel)
    {
        return;
    }

    struct wd_view* view = view_from_xdg_toplevel(server, event->toplevel);
    if (!view)
    {
        WD_LOG_DEBUG("xdg-toplevel-icon for unknown toplevel=%p", (void*)event->toplevel);
        return;
    }

    if (view->toplevel_icon)
    {
        wlr_xdg_toplevel_icon_v1_unref(view->toplevel_icon);
        view->toplevel_icon = NULL;
    }

    if (event->icon)
    {
        view->toplevel_icon = wlr_xdg_toplevel_icon_v1_ref(event->icon);

        WD_LOG_DEBUG("xdg-toplevel-icon set view=%p name=%s buffers=%zu", (void*)view,
                     view->toplevel_icon->name ? view->toplevel_icon->name : "(none)", icon_buffer_count(view->toplevel_icon));
    }
    else
    {
        WD_LOG_DEBUG("xdg-toplevel-icon cleared view=%p", (void*)view);
    }
}

bool wd_xdg_toplevel_icon_init(struct wd_server* server) {
    if (!server || !server->display)
    {
        return false;
    }

    server->xdg_toplevel_icon_manager = wlr_xdg_toplevel_icon_manager_v1_create(server->display, 1);
    if (!server->xdg_toplevel_icon_manager)
    {
        WD_LOG_ERROR("failed to create xdg-toplevel-icon manager");
        return false;
    }

    /*
     * Advertise common shell/taskbar sizes. WayDisplay does not render a
     * taskbar yet, but these preferences help clients choose sensible buffers
     * if/when we expose icons in a future shell UI.
     */
    int preferred_sizes[] = {
        16, 24, 32, 48, 64, 128, 256,
    };

    wlr_xdg_toplevel_icon_manager_v1_set_sizes(server->xdg_toplevel_icon_manager, preferred_sizes,
                                               sizeof(preferred_sizes) / sizeof(preferred_sizes[0]));

    wl_list_init(&server->set_xdg_toplevel_icon.link);
    server->set_xdg_toplevel_icon.notify = handle_set_xdg_toplevel_icon;
    wl_signal_add(&server->xdg_toplevel_icon_manager->events.set_icon, &server->set_xdg_toplevel_icon);

    WD_LOG_INFO("xdg-toplevel-icon enabled");
    return true;
}

void wd_xdg_toplevel_icon_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    remove_listener_if_linked(&server->set_xdg_toplevel_icon);

    struct wd_view* view = NULL;
    wl_list_for_each(view, &server->views, link) {
        if (view->toplevel_icon)
        {
            wlr_xdg_toplevel_icon_v1_unref(view->toplevel_icon);
            view->toplevel_icon = NULL;
        }
    }

    server->xdg_toplevel_icon_manager = NULL;
}
