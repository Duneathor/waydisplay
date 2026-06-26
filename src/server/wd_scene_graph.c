#include "wd_scene_graph.h"

#include "wd_server_internal.h"

static bool view_is_linked_to_server(const struct wd_view* view) {
    if (!view || !view->server || !view->server->views.prev || !view->server->views.next || !view->link.prev || !view->link.next)
    {
        return false;
    }

    struct wd_view* candidate = NULL;
    wl_list_for_each(candidate, &view->server->views, link) {
        if (candidate == view)
        {
            return true;
        }
    }

    return false;
}

struct wd_view* wd_scene_graph_view_from_node(struct wd_server* server, struct wlr_scene_node* node) {
    if (!server || !server->views.prev || !server->views.next)
    {
        return NULL;
    }

    while (node)
    {
        if (node->data)
        {
            struct wd_view* view = NULL;
            wl_list_for_each(view, &server->views, link) {
                if (node->data == view)
                {
                    return view;
                }
            }
        }

        if (!node->parent)
        {
            break;
        }

        node = &node->parent->node;
    }

    return NULL;
}

bool wd_scene_graph_set_view_position(struct wd_view* view) {
    if (!view || !view->scene_tree)
    {
        return false;
    }

    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    return true;
}

bool wd_scene_graph_raise_view(struct wd_view* view) {
    if (!view || !view->scene_tree || !view_is_linked_to_server(view))
    {
        return false;
    }

    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(view->server->views.prev, &view->link);
    return true;
}
