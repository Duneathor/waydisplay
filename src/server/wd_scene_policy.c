#include "wd_scene_policy.h"

void wd_scene_policy_popup_constraint_box(int view_x, int view_y, int root_geometry_x, int root_geometry_y, int output_width,
                                          int output_height, struct wd_scene_policy_box* out_box) {
    if (!out_box)
    {
        return;
    }

    /* XDG popup constraints are expressed in root-toplevel surface
     * coordinates, while WayDisplay stores the root window geometry origin in
     * output-layout coordinates. */
    out_box->x      = -view_x + root_geometry_x;
    out_box->y      = -view_y + root_geometry_y;
    out_box->width  = output_width;
    out_box->height = output_height;
}

bool wd_scene_policy_focus_candidate(bool mapped, bool minimized, bool override_redirect, bool excluded) {
    return mapped && !minimized && !override_redirect && !excluded;
}
