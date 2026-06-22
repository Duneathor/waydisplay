#pragma once

#include <stdbool.h>

struct wd_server;
struct wd_view;
struct wlr_scene_node;

#ifdef __cplusplus
extern "C" {
#endif

struct wd_view* wd_scene_graph_view_from_node(struct wd_server* server, struct wlr_scene_node* node);
bool            wd_scene_graph_set_view_position(struct wd_view* view);
bool            wd_scene_graph_raise_view(struct wd_view* view);

#ifdef __cplusplus
}
#endif
