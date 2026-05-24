#include "wd_server.h"

#include <stdint.h>

#include "xdg-dialog-v1-protocol.h"

static void xdg_dialog_resource_destroy(struct wl_resource *resource) {
    struct wd_view *view = wl_resource_get_user_data(resource);

    if (!view) {
        return;
    }

    if (view->xdg_dialog_resource == resource) {
        view->xdg_dialog_resource = NULL;
    }

    view->is_dialog = false;
    view->dialog_modal = false;
    wd_scene_note_dialog_state(view);
}

static void xdg_dialog_handle_destroy(struct wl_client *client,
                                      struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_dialog_handle_set_modal(struct wl_client *client,
                                        struct wl_resource *resource) {
    (void)client;

    struct wd_view *view = wl_resource_get_user_data(resource);
    if (!view) {
        return;
    }

    view->is_dialog = true;
    view->dialog_modal = true;
    wd_scene_note_dialog_state(view);
}

static void xdg_dialog_handle_unset_modal(struct wl_client *client,
                                          struct wl_resource *resource) {
    (void)client;

    struct wd_view *view = wl_resource_get_user_data(resource);
    if (!view) {
        return;
    }

    view->is_dialog = true;
    view->dialog_modal = false;
    wd_scene_note_dialog_state(view);
}

static const struct xdg_dialog_v1_interface xdg_dialog_impl = {
    .destroy = xdg_dialog_handle_destroy,
    .set_modal = xdg_dialog_handle_set_modal,
    .unset_modal = xdg_dialog_handle_unset_modal,
};

static void xdg_wm_dialog_handle_destroy(struct wl_client *client,
                                         struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_wm_dialog_handle_get_xdg_dialog(
    struct wl_client *client,
    struct wl_resource *resource,
    uint32_t id,
    struct wl_resource *toplevel_resource) {
    struct wd_server *server = wl_resource_get_user_data(resource);

    if (!server || !toplevel_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    struct wd_view *view =
        wd_scene_view_from_xdg_toplevel_resource(server, toplevel_resource);

    if (view && view->xdg_dialog_resource) {
        wl_resource_post_error(resource,
                               XDG_WM_DIALOG_V1_ERROR_ALREADY_USED,
                               "xdg_toplevel already has an xdg_dialog_v1");
        return;
    }

    struct wl_resource *dialog_resource =
        wl_resource_create(client,
                           &xdg_dialog_v1_interface,
                           wl_resource_get_version(resource),
                           id);
    if (!dialog_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }

    /*
     * If the toplevel is unknown to WayDisplay, keep the dialog resource inert
     * instead of crashing. This should not happen for normal xdg-shell clients,
     * but it is safer for protocol edge cases during teardown.
     */
    wl_resource_set_implementation(dialog_resource,
                                   &xdg_dialog_impl,
                                   view,
                                   xdg_dialog_resource_destroy);

    if (!view) {
        wlr_log(WLR_DEBUG,
                "WayDisplay: xdg-dialog for unknown toplevel resource=%p",
                (void *)toplevel_resource);
        return;
    }

    view->xdg_dialog_resource = dialog_resource;
    view->is_dialog = true;
    view->dialog_modal = false;

    wd_scene_note_dialog_state(view);
}

static const struct xdg_wm_dialog_v1_interface xdg_wm_dialog_impl = {
    .destroy = xdg_wm_dialog_handle_destroy,
    .get_xdg_dialog = xdg_wm_dialog_handle_get_xdg_dialog,
};

static void bind_xdg_dialog_manager(struct wl_client *client,
                                    void *data,
                                    uint32_t version,
                                    uint32_t id) {
    struct wd_server *server = data;

    struct wl_resource *resource =
        wl_resource_create(client,
                           &xdg_wm_dialog_v1_interface,
                           version,
                           id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource,
                                   &xdg_wm_dialog_impl,
                                   server,
                                   NULL);
}

bool wd_xdg_dialog_init(struct wd_server *server) {
    if (!server || !server->display) {
        return false;
    }

    server->xdg_dialog_manager_global =
        wl_global_create(server->display,
                         &xdg_wm_dialog_v1_interface,
                         1,
                         server,
                         bind_xdg_dialog_manager);
    if (!server->xdg_dialog_manager_global) {
        wlr_log(WLR_ERROR,
                "WayDisplay: failed to create xdg-dialog manager");
        return false;
    }

    wlr_log(WLR_INFO, "WayDisplay: xdg-dialog enabled");
    return true;
}

void wd_xdg_dialog_destroy(struct wd_server *server) {
    if (!server) {
        return;
    }

    if (server->xdg_dialog_manager_global) {
        wl_global_destroy(server->xdg_dialog_manager_global);
        server->xdg_dialog_manager_global = NULL;
    }

    struct wd_view *view = NULL;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_dialog_resource) {
            wl_resource_set_user_data(view->xdg_dialog_resource, NULL);
            view->xdg_dialog_resource = NULL;
        }

        view->is_dialog = false;
        view->dialog_modal = false;
    }
}
