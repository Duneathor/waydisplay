#include "wd_server.h"

#include <string.h>

#include "waydisplay/wd_net.h"

static void remove_listener_if_linked(struct wl_listener *listener) {
    if (!listener) {
        return;
    }

    if (listener->link.prev && listener->link.next) {
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }
}

static uint16_t wd_shape_from_wp_shape(
    enum wp_cursor_shape_device_v1_shape shape) {
    switch (shape) {
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER:
            return WD_CURSOR_SHAPE_POINTER;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT:
            return WD_CURSOR_SHAPE_TEXT;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL:
            return WD_CURSOR_SHAPE_MOVE;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE:
            return WD_CURSOR_SHAPE_EW_RESIZE;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE:
            return WD_CURSOR_SHAPE_NS_RESIZE;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE:
            return WD_CURSOR_SHAPE_NWSE_RESIZE;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE:
            return WD_CURSOR_SHAPE_NESW_RESIZE;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT:
            return WD_CURSOR_SHAPE_WAIT;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP:
        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED:
            return WD_CURSOR_SHAPE_NOT_ALLOWED;

        case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT:
        default:
            return WD_CURSOR_SHAPE_DEFAULT;
    }
}

static bool wd_cursor_send_shape_locked(struct wd_server *server,
                                        uint16_t shape) {
    if (!server) {
        return false;
    }

    struct wd_net_state *net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0 || net->session_id == 0) {
        return true;
    }

    struct wd_cursor_shape_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.session_id = net->session_id;
    payload.shape = shape;

    return wd_send_tcp_message(net->tcp_fd,
                               WD_MSG_CURSOR_SHAPE,
                               &payload,
                               sizeof(payload));
}

bool wd_cursor_send_current_locked(struct wd_server *server) {
    if (!server) {
        return false;
    }

    return wd_cursor_send_shape_locked(server, server->cursor_shape);
}

void wd_cursor_set_shape(struct wd_server *server, uint16_t shape) {
    if (!server) {
        return;
    }

    if (shape >= WD_CURSOR_SHAPE_COUNT) {
        shape = WD_CURSOR_SHAPE_DEFAULT;
    }

    if (server->cursor_shape == shape) {
        return;
    }

    server->cursor_shape = shape;

    pthread_mutex_lock(&server->net.lock);
    wd_cursor_send_shape_locked(server, shape);
    pthread_mutex_unlock(&server->net.lock);
}

uint16_t wd_cursor_shape_for_resize_edges(uint32_t edges) {
    const bool left = (edges & WLR_EDGE_LEFT) != 0;
    const bool right = (edges & WLR_EDGE_RIGHT) != 0;
    const bool top = (edges & WLR_EDGE_TOP) != 0;
    const bool bottom = (edges & WLR_EDGE_BOTTOM) != 0;

    if ((left && top) || (right && bottom)) {
        return WD_CURSOR_SHAPE_NWSE_RESIZE;
    }

    if ((right && top) || (left && bottom)) {
        return WD_CURSOR_SHAPE_NESW_RESIZE;
    }

    if (left || right) {
        return WD_CURSOR_SHAPE_EW_RESIZE;
    }

    if (top || bottom) {
        return WD_CURSOR_SHAPE_NS_RESIZE;
    }

    return WD_CURSOR_SHAPE_DEFAULT;
}

static void handle_cursor_shape_request(struct wl_listener *listener,
                                        void *data) {
    struct wd_server *server =
        wl_container_of(listener, server, request_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

    if (!server || !event) {
        return;
    }

    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
        return;
    }

    /*
     * In a normal hardware compositor this would load an XCursor locally. For
     * WayDisplay, relay the semantic cursor shape to the SDL viewer so the
     * user's host cursor changes.
     */
    uint16_t shape = wd_shape_from_wp_shape(event->shape);
    wd_cursor_set_shape(server, shape);

    WD_LOG_DEBUG(
            "WayDisplay: client requested cursor-shape=%s mapped=%u",
            wlr_cursor_shape_v1_name(event->shape),
            shape);
}

bool wd_cursor_init(struct wd_server *server) {
    if (!server || !server->display) {
        return false;
    }

    server->cursor_shape = WD_CURSOR_SHAPE_DEFAULT;

    server->cursor_shape_manager =
        wlr_cursor_shape_manager_v1_create(server->display, 1);
    if (!server->cursor_shape_manager) {
        WD_LOG_ERROR(
                "WayDisplay: failed to create cursor-shape manager");
        return false;
    }

    wl_list_init(&server->request_cursor_shape.link);
    server->request_cursor_shape.notify = handle_cursor_shape_request;
    wl_signal_add(&server->cursor_shape_manager->events.request_set_shape,
                  &server->request_cursor_shape);

    WD_LOG_INFO( "WayDisplay: cursor-shape enabled");
    return true;
}

void wd_cursor_destroy(struct wd_server *server) {
    if (!server) {
        return;
    }

    remove_listener_if_linked(&server->request_cursor_shape);
    server->cursor_shape_manager = NULL;
}
