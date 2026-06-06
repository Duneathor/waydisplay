#include "wd_server.h"

#include <string.h>

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

static struct wd_view* view_from_toplevel_surface(struct wd_server* server, struct wlr_surface* surface) {
    if (!server || !surface)
    {
        return NULL;
    }

    struct wd_view* view = NULL;

    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_surface || !view->xdg_surface->surface)
        {
            continue;
        }

        if (view->xdg_surface->surface == surface)
        {
            return view;
        }
    }

    return NULL;
}

static bool token_is_reasonable_for_request(struct wlr_xdg_activation_v1_request_activate_event* event) {
    if (!event)
    {
        return false;
    }

    /*
     * Be intentionally permissive for now.
     *
     * xdg-activation is primarily a focus-stealing mitigation protocol, but
     * WayDisplay currently has no launcher/taskbar policy layer and only one
     * remote user/session. wlroots has already parsed and tracked the token for
     * us; accepting valid request_activate events is enough to make portals,
     * launchers, browsers, and "open URL in existing app" flows work.
     *
     * Later, this is the right place to enforce stricter policy, for example:
     *   - require token->seat == server->seat when token->seat is non-NULL
     *   - require token->surface to match the currently focused surface
     *   - reject stale app_id mismatches
     */
    return event->surface != NULL;
}

static void handle_request_activate(struct wl_listener* listener, void* data) {
    struct wd_server*                                    server = wl_container_of(listener, server, request_activate);
    struct wlr_xdg_activation_v1_request_activate_event* event  = data;

    if (!server || !event || !token_is_reasonable_for_request(event))
    {
        return;
    }

    struct wd_view* view = view_from_toplevel_surface(server, event->surface);

    if (!view)
    {
        WD_LOG_DEBUG("WayDisplay: xdg-activation requested unknown surface=%p", (void*)event->surface);
        return;
    }

    if (!view->mapped)
    {
        /*
         * Some clients request activation before the initial map completes.
         * The map handler already focuses new mapped toplevels, so there is no
         * extra pending-activation state needed here yet.
         */
        WD_LOG_DEBUG("WayDisplay: xdg-activation requested unmapped view=%p", (void*)view);
        return;
    }

    wd_scene_focus_view(view);

    if (event->token)
    {
        const char* token_name = wlr_xdg_activation_token_v1_get_name(event->token);

        WD_LOG_INFO("WayDisplay: xdg-activation focused view=%p token=%s", (void*)view, token_name ? token_name : "(null)");

        /*
         * Do not destroy event->token here. wlroots owns activation token
         * lifetime and may already have unlinked the token by the time the
         * request_activate handler runs. Firefox's profile wizard can trigger
         * this path while creating a new toplevel; explicitly destroying the
         * token here can double-remove its wl_list link and crash in
         * wl_list_remove().
         */
    }
    else
    {
        WD_LOG_INFO("WayDisplay: xdg-activation focused view=%p without token", (void*)view);
    }
}

bool wd_xdg_activation_init(struct wd_server* server) {
    if (!server || !server->display)
    {
        return false;
    }

    server->xdg_activation = wlr_xdg_activation_v1_create(server->display);
    if (!server->xdg_activation)
    {
        WD_LOG_ERROR("WayDisplay: failed to create xdg-activation manager");
        return false;
    }

    /*
     * Keep tokens alive long enough for common launcher/browser handoffs while
     * still limiting stale focus-stealing attempts.
     */
    server->xdg_activation->token_timeout_msec = WD_XDG_ACTIVATION_TOKEN_TIMEOUT_MS;

    wl_list_init(&server->request_activate.link);
    server->request_activate.notify = handle_request_activate;
    wl_signal_add(&server->xdg_activation->events.request_activate, &server->request_activate);

    WD_LOG_INFO("WayDisplay: xdg-activation enabled");
    return true;
}

void wd_xdg_activation_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    remove_listener_if_linked(&server->request_activate);
    server->request_activate.notify = NULL;

    server->xdg_activation = NULL;
}
