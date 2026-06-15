#include "waydisplay/wd_net.h"
#include "wd_server.h"
#include "wd_async_tcp.h"

#include <string.h>
#include <sys/socket.h>

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

static uint16_t wd_shape_from_wp_shape(enum wp_cursor_shape_device_v1_shape shape) {
    switch (shape)
    {
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

static bool wd_cursor_send_shape_locked(struct wd_server* server, uint16_t shape) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;

    if (!net->client_connected || net->tcp_fd < 0 || net->session_id == 0)
    {
        return true;
    }

    struct wd_cursor_shape_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.session_id = net->session_id;
    payload.shape      = shape;

    bool ok = false;
    if (net->control_tx)
    {
        ok = wd_async_tcp_send_message(net->control_tx, net->tcp_fd, WD_MSG_CURSOR_SHAPE, &payload, sizeof(payload));
        if (!ok)
        {
            net->stats.tcp_async_send_failed++;
            if (net->tcp_fd >= 0)
            {
                (void)shutdown(net->tcp_fd, SHUT_RDWR);
            }
        }
    }
    else
    {
        ok = wd_send_tcp_message(net->tcp_fd, WD_MSG_CURSOR_SHAPE, &payload, sizeof(payload));
    }

    if (ok)
    {
        wd_stream_account_tcp_control_bytes_locked(net,
                                                   (uint32_t)(sizeof(struct wd_tcp_header) + sizeof(payload)));
        net->stats.cursor_shape_tx++;
    }
    return ok;
}

void wd_cursor_queue_current_locked(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    struct wd_net_state* net = &server->net;
    if (net->pending_cursor_shape_dirty && net->pending_cursor_shape != server->cursor_shape)
    {
        net->stats.cursor_shape_coalesced++;
    }
    net->pending_cursor_shape       = server->cursor_shape;
    net->pending_cursor_shape_dirty = true;
}

bool wd_cursor_flush_pending_locked(struct wd_server* server) {
    if (!server)
    {
        return false;
    }

    struct wd_net_state* net = &server->net;
    if (!net->pending_cursor_shape_dirty)
    {
        return true;
    }

    uint16_t shape = net->pending_cursor_shape;
    net->pending_cursor_shape_dirty = false;

    bool ok = wd_cursor_send_shape_locked(server, shape);
    if (!ok)
    {
        net->pending_cursor_shape       = shape;
        net->pending_cursor_shape_dirty = true;
    }
    return ok;
}

void wd_cursor_set_shape(struct wd_server* server, uint16_t shape) {
    if (!server)
    {
        return;
    }

    if (shape >= WD_CURSOR_SHAPE_COUNT)
    {
        shape = WD_CURSOR_SHAPE_DEFAULT;
    }

    if (server->cursor_shape == shape)
    {
        return;
    }

    server->cursor_shape = shape;

    pthread_mutex_lock(&server->net.lock);
    wd_cursor_queue_current_locked(server);
    pthread_mutex_unlock(&server->net.lock);
}

uint16_t wd_cursor_shape_for_resize_edges(uint32_t edges) {
    const bool left   = (edges & WLR_EDGE_LEFT) != 0;
    const bool right  = (edges & WLR_EDGE_RIGHT) != 0;
    const bool top    = (edges & WLR_EDGE_TOP) != 0;
    const bool bottom = (edges & WLR_EDGE_BOTTOM) != 0;

    if ((left && top) || (right && bottom))
    {
        return WD_CURSOR_SHAPE_NWSE_RESIZE;
    }

    if ((right && top) || (left && bottom))
    {
        return WD_CURSOR_SHAPE_NESW_RESIZE;
    }

    if (left || right)
    {
        return WD_CURSOR_SHAPE_EW_RESIZE;
    }

    if (top || bottom)
    {
        return WD_CURSOR_SHAPE_NS_RESIZE;
    }

    return WD_CURSOR_SHAPE_DEFAULT;
}

static void handle_cursor_shape_request(struct wl_listener* listener, void* data) {
    struct wd_server*                                           server = wl_container_of(listener, server, request_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event* event  = data;

    if (!server || !event)
    {
        return;
    }

    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER)
    {
        return;
    }

    /*
     * In a normal hardware compositor this would load an XCursor locally. For
     * WayDisplay, relay the semantic cursor shape to the SDL viewer so the
     * user's host cursor changes.
     */
    server->net.stats.cursor_shape_requests++;

    uint16_t shape     = wd_shape_from_wp_shape(event->shape);
    bool     changed   = server->cursor_shape != shape;
    wd_cursor_set_shape(server, shape);

    if (changed)
    {
        WD_LOG_DEBUG("client requested cursor-shape=%s mapped=%u", wlr_cursor_shape_v1_name(event->shape), shape);
    }
}


static bool wd_cursor_request_client_has_pointer_focus(struct wd_server* server, struct wlr_seat_client* seat_client) {
    if (!server || !server->seat || !seat_client)
    {
        return false;
    }

    struct wlr_seat_client* focused_client = server->seat->pointer_state.focused_client;
    return focused_client && focused_client->client == seat_client->client;
}

static void handle_request_set_cursor(struct wl_listener* listener, void* data) {
    struct wd_server*                                server = wl_container_of(listener, server, request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event* event = data;

    if (!server || !event)
    {
        return;
    }

    server->net.stats.cursor_set_cursor_requests++;

    if (!wd_cursor_request_client_has_pointer_focus(server, event->seat_client))
    {
        server->net.stats.cursor_set_cursor_rejected++;
        WD_LOG_DEBUG("rejected wl_pointer.set_cursor from unfocused client");
        return;
    }

    if (!event->surface)
    {
        server->net.stats.cursor_set_cursor_hidden++;
        wd_cursor_set_shape(server, WD_CURSOR_SHAPE_HIDDEN);
        return;
    }

    /*
     * Classic wl_pointer.set_cursor carries an arbitrary client-supplied
     * surface. WayDisplay does not yet transport cursor-surface pixels to the
     * SDL viewer, so accept the request for focus/serial correctness and use a
     * neutral host cursor instead of leaving a stale compositor-chosen shape.
     * A later cursor-image protocol can replace this fallback with the actual
     * surface contents and hotspot.
     */
    server->net.stats.cursor_set_cursor_fallback++;
    wd_cursor_set_shape(server, WD_CURSOR_SHAPE_DEFAULT);
}

bool wd_cursor_init(struct wd_server* server) {
    if (!server || !server->display || !server->seat)
    {
        return false;
    }

    server->cursor_shape = WD_CURSOR_SHAPE_DEFAULT;

    server->cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
    if (!server->cursor_shape_manager)
    {
        WD_LOG_ERROR("failed to create cursor-shape manager");
        return false;
    }

    wl_list_init(&server->request_cursor_shape.link);
    server->request_cursor_shape.notify = handle_cursor_shape_request;
    wl_signal_add(&server->cursor_shape_manager->events.request_set_shape, &server->request_cursor_shape);

    wl_list_init(&server->request_set_cursor.link);
    server->request_set_cursor.notify = handle_request_set_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_set_cursor);

    WD_LOG_DEBUG("cursor-shape and wl_pointer.set_cursor enabled");
    return true;
}

void wd_cursor_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    remove_listener_if_linked(&server->request_cursor_shape);
    remove_listener_if_linked(&server->request_set_cursor);
    server->cursor_shape_manager = NULL;
}
