#include <stdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

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

struct destroy_probe {
    struct wl_listener listener;
    int*               count;
};

static void handle_destroy(struct wl_listener* listener, void* data) {
    (void)data;
    struct destroy_probe* probe = wl_container_of(listener, probe, listener);
    ++*probe->count;
    wl_list_remove(&probe->listener.link);
    wl_list_init(&probe->listener.link);
}

static void add_destroy_probe(struct wlr_scene_node* node, struct destroy_probe* probe, int* count) {
    memset(probe, 0, sizeof(*probe));
    probe->count           = count;
    probe->listener.notify = handle_destroy;
    wl_signal_add(&node->events.destroy, &probe->listener);
}

static struct wlr_scene_node* node_at(struct wlr_scene* scene, double x, double y, double* nx, double* ny) {
    *nx = 0.0;
    *ny = 0.0;
    return wlr_scene_node_at(&scene->tree.node, x, y, nx, ny);
}

static void test_reparent_enable_and_coordinates(void) {
    struct wlr_scene* scene = wlr_scene_create();
    CHECK(scene != NULL);
    if (!scene)
    {
        return;
    }

    struct wlr_scene_tree* left     = wlr_scene_tree_create(&scene->tree);
    struct wlr_scene_tree* right    = wlr_scene_tree_create(&scene->tree);
    const float            color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    struct wlr_scene_rect* rect     = wlr_scene_rect_create(left, 20, 20, color);
    CHECK(left != NULL && right != NULL && rect != NULL);

    wlr_scene_node_set_position(&left->node, 10, 20);
    wlr_scene_node_set_position(&right->node, 100, 120);
    wlr_scene_node_set_position(&rect->node, 3, 4);

    int lx = 0;
    int ly = 0;
    CHECK(wlr_scene_node_coords(&rect->node, &lx, &ly));
    CHECK(lx == 13 && ly == 24);

    double nx = 0.0;
    double ny = 0.0;
    CHECK(node_at(scene, 15.0, 27.0, &nx, &ny) == &rect->node);
    CHECK(nx == 2.0 && ny == 3.0);

    wlr_scene_node_reparent(&rect->node, right);
    CHECK(wlr_scene_node_coords(&rect->node, &lx, &ly));
    CHECK(lx == 103 && ly == 124);
    CHECK(node_at(scene, 15.0, 27.0, &nx, &ny) == NULL);
    CHECK(node_at(scene, 105.0, 127.0, &nx, &ny) == &rect->node);

    wlr_scene_node_set_enabled(&right->node, false);
    CHECK(node_at(scene, 105.0, 127.0, &nx, &ny) == NULL);
    wlr_scene_node_set_enabled(&right->node, true);
    CHECK(node_at(scene, 105.0, 127.0, &nx, &ny) == &rect->node);

    wlr_scene_node_destroy(&scene->tree.node);
}

static void test_recursive_destruction_signals_once(void) {
    struct wlr_scene* scene = wlr_scene_create();
    CHECK(scene != NULL);
    if (!scene)
    {
        return;
    }

    struct wlr_scene_tree* parent   = wlr_scene_tree_create(&scene->tree);
    struct wlr_scene_tree* child    = wlr_scene_tree_create(parent);
    const float            color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    struct wlr_scene_rect* rect     = wlr_scene_rect_create(child, 8, 8, color);
    CHECK(parent != NULL && child != NULL && rect != NULL);

    int                  parent_destroyed = 0;
    int                  child_destroyed  = 0;
    int                  rect_destroyed   = 0;
    struct destroy_probe parent_probe;
    struct destroy_probe child_probe;
    struct destroy_probe rect_probe;
    add_destroy_probe(&parent->node, &parent_probe, &parent_destroyed);
    add_destroy_probe(&child->node, &child_probe, &child_destroyed);
    add_destroy_probe(&rect->node, &rect_probe, &rect_destroyed);

    wlr_scene_node_destroy(&parent->node);
    CHECK(parent_destroyed == 1);
    CHECK(child_destroyed == 1);
    CHECK(rect_destroyed == 1);

    wlr_scene_node_destroy(&scene->tree.node);
    CHECK(parent_destroyed == 1);
    CHECK(child_destroyed == 1);
    CHECK(rect_destroyed == 1);
}

int main(void) {
    test_reparent_enable_and_coordinates();
    test_recursive_destruction_signals_once();

    if (failures != 0)
    {
        fprintf(stderr, "wlroots scene lifecycle tests failed: %d\n", failures);
        return 1;
    }

    return 0;
}
