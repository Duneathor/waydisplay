#include "wd_scene_graph.h"
#include "wd_server_internal.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);                                                       \
            ++failures;                                                                                                                    \
        }                                                                                                                                  \
    } while (0)

static void init_server(struct wd_server* server) {
    memset(server, 0, sizeof(*server));
    wl_list_init(&server->views);
}

static void init_view(struct wd_view* view, struct wd_server* server, struct wlr_scene_tree* tree) {
    memset(view, 0, sizeof(*view));
    view->server     = server;
    view->scene_tree = tree;
    wl_list_insert(server->views.prev, &view->link);
    tree->node.data = view;
}

static struct wlr_scene_node* node_at(struct wlr_scene* scene, double x, double y) {
    double nx = 0.0;
    double ny = 0.0;
    return wlr_scene_node_at(&scene->tree.node, x, y, &nx, &ny);
}

static void test_ancestry_lookup_and_identity_validation(void) {
    struct wd_server server;
    init_server(&server);

    struct wlr_scene* scene = wlr_scene_create();
    CHECK(scene != NULL);
    if (!scene)
    {
        return;
    }

    struct wlr_scene_tree* view_tree = wlr_scene_tree_create(&scene->tree);
    struct wlr_scene_tree* child     = wlr_scene_tree_create(view_tree);
    const float            color[4]  = {1.0f, 0.0f, 0.0f, 1.0f};
    struct wlr_scene_rect* rect      = wlr_scene_rect_create(child, 32, 32, color);
    CHECK(view_tree != NULL && child != NULL && rect != NULL);

    struct wd_view view;
    init_view(&view, &server, view_tree);

    CHECK(wd_scene_graph_view_from_node(&server, &rect->node) == &view);
    CHECK(wd_scene_graph_view_from_node(&server, &child->node) == &view);
    CHECK(wd_scene_graph_view_from_node(NULL, &rect->node) == NULL);
    CHECK(wd_scene_graph_view_from_node(&server, NULL) == NULL);

    struct wd_view forged;
    memset(&forged, 0, sizeof(forged));
    rect->node.data = &forged;
    CHECK(wd_scene_graph_view_from_node(&server, &rect->node) == &view);

    struct wlr_scene_tree* unrelated = wlr_scene_tree_create(&scene->tree);
    CHECK(unrelated != NULL);
    if (unrelated)
    {
        unrelated->node.data = &forged;
        CHECK(wd_scene_graph_view_from_node(&server, &unrelated->node) == NULL);
    }

    wl_list_remove(&view.link);
    wl_list_init(&view.link);
    CHECK(wd_scene_graph_view_from_node(&server, &rect->node) == NULL);

    wlr_scene_node_destroy(&scene->tree.node);
}

static void test_position_raise_and_hit_order(void) {
    struct wd_server server;
    init_server(&server);

    struct wlr_scene* scene = wlr_scene_create();
    CHECK(scene != NULL);
    if (!scene)
    {
        return;
    }

    struct wlr_scene_tree* tree_a  = wlr_scene_tree_create(&scene->tree);
    struct wlr_scene_tree* tree_b  = wlr_scene_tree_create(&scene->tree);
    const float            red[4]  = {1.0f, 0.0f, 0.0f, 1.0f};
    const float            blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    struct wlr_scene_rect* rect_a  = wlr_scene_rect_create(tree_a, 80, 80, red);
    struct wlr_scene_rect* rect_b  = wlr_scene_rect_create(tree_b, 80, 80, blue);
    CHECK(tree_a != NULL && tree_b != NULL && rect_a != NULL && rect_b != NULL);

    struct wd_view a;
    struct wd_view b;
    init_view(&a, &server, tree_a);
    init_view(&b, &server, tree_b);

    a.x = 12;
    a.y = 18;
    b.x = 12;
    b.y = 18;
    CHECK(wd_scene_graph_set_view_position(&a));
    CHECK(wd_scene_graph_set_view_position(&b));
    CHECK(tree_a->node.x == 12 && tree_a->node.y == 18);
    CHECK(tree_b->node.x == 12 && tree_b->node.y == 18);

    CHECK(node_at(scene, 20.0, 24.0) == &rect_b->node);
    CHECK(server.views.prev == &b.link);

    CHECK(wd_scene_graph_raise_view(&a));
    CHECK(node_at(scene, 20.0, 24.0) == &rect_a->node);
    CHECK(server.views.prev == &a.link);

    wlr_scene_node_set_enabled(&tree_a->node, false);
    CHECK(node_at(scene, 20.0, 24.0) == &rect_b->node);
    wlr_scene_node_set_enabled(&tree_a->node, true);
    CHECK(node_at(scene, 20.0, 24.0) == &rect_a->node);

    struct wd_view unlinked;
    memset(&unlinked, 0, sizeof(unlinked));
    unlinked.server     = &server;
    unlinked.scene_tree = tree_b;
    wl_list_init(&unlinked.link);
    CHECK(!wd_scene_graph_raise_view(&unlinked));
    CHECK(server.views.prev == &a.link);

    CHECK(!wd_scene_graph_set_view_position(NULL));
    unlinked.scene_tree = NULL;
    CHECK(!wd_scene_graph_set_view_position(&unlinked));

    wl_list_remove(&a.link);
    wl_list_remove(&b.link);
    wlr_scene_node_destroy(&scene->tree.node);
}

int main(void) {
    test_ancestry_lookup_and_identity_validation();
    test_position_raise_and_hit_order();

    if (failures != 0)
    {
        fprintf(stderr, "wlroots scene graph tests failed: %d\n", failures);
        return 1;
    }

    return 0;
}
