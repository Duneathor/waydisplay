#ifndef WAYDISPLAY_WD_SCENE_POLICY_H
#define WAYDISPLAY_WD_SCENE_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_scene_policy_box {
    int x;
    int y;
    int width;
    int height;
};

void wd_scene_policy_popup_constraint_box(int view_x, int view_y, int root_geometry_x, int root_geometry_y, int output_width,
                                          int output_height, struct wd_scene_policy_box* out_box);

bool wd_scene_policy_focus_candidate(bool mapped, bool minimized, bool override_redirect, bool excluded);

#ifdef __cplusplus
}
#endif

#endif
