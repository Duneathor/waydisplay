#pragma once

#include "waydisplay/wd_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_server;
struct wd_view;
struct wl_resource;
struct wlr_scene_node;
struct wlr_surface;

bool wd_wlroots_init(struct wd_server* server);
bool wd_wlroots_start(struct wd_server* server);
bool wd_wlroots_create_headless_output(struct wd_server* server);
bool wd_wlroots_resize_headless_output(struct wd_server* server);

#if WAYDISPLAY_ENABLE_XWAYLAND
bool wd_xwayland_init(struct wd_server* server);
void wd_xwayland_destroy(struct wd_server* server);
bool wd_xwayland_view_has_decoration(struct wd_view* view);
void wd_xwayland_view_update_scene_position(struct wd_view* view);
bool wd_xwayland_view_decoration_at(struct wd_view* view, double sx, double sy);
bool wd_xwayland_view_handle_decoration_press(struct wd_view* view, double sx, double sy);
void wd_xwayland_handle_output_resize(struct wd_server* server);
#endif

bool wd_xdg_decoration_init(struct wd_server* server);
void wd_xdg_decoration_destroy(struct wd_server* server);
bool wd_xdg_activation_init(struct wd_server* server);
void wd_xdg_activation_destroy(struct wd_server* server);
bool wd_xdg_foreign_init(struct wd_server* server);
void wd_xdg_foreign_destroy(struct wd_server* server);
bool wd_xdg_dialog_init(struct wd_server* server);
void wd_xdg_dialog_destroy(struct wd_server* server);
bool wd_xdg_toplevel_icon_init(struct wd_server* server);
void wd_xdg_toplevel_icon_destroy(struct wd_server* server);

bool wd_keyboard_shortcuts_inhibit_init(struct wd_server* server);
void wd_keyboard_shortcuts_inhibit_destroy(struct wd_server* server);
void wd_keyboard_shortcuts_inhibit_refresh(struct wd_server* server);
bool wd_keyboard_shortcuts_inhibit_active(struct wd_server* server);

void            wd_scene_init_listeners(struct wd_server* server);
void            wd_scene_focus_view(struct wd_view* view);
void            wd_scene_focus_topmost(struct wd_server* server, struct wd_view* exclude);
void            wd_scene_deactivate_view(struct wd_view* view);
void            wd_scene_set_view_position(struct wd_view* view);
void            wd_scene_note_dialog_state(struct wd_view* view);
void            wd_scene_handle_output_resize(struct wd_server* server);
struct wd_view* wd_scene_view_from_xdg_toplevel_resource(struct wd_server* server, struct wl_resource* toplevel_resource);
struct wd_view* wd_scene_view_from_node(struct wd_server* server, struct wlr_scene_node* node);
void            wd_scene_raise_view(struct wd_view* view);
void            wd_server_request_full_refresh(struct wd_server* server);
void            wd_server_clear_scene_damage(struct wd_server* server);

enum wd_render_result {
    WD_RENDER_RESULT_ERROR = -1,
    WD_RENDER_RESULT_IDLE  = 0,
    WD_RENDER_RESULT_FRAME = 1,
};

enum wd_render_result wd_render_scene_and_readback_xrgb8888(struct wd_server* server);

#ifdef __cplusplus
}
#endif
