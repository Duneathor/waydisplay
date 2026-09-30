if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_xwayland.c" xwayland_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_pointer.c" pointer_source)

foreach(required
        "xwayland_view_set_minimized(view, true);"
        "wlr_scene_node_set_enabled(&view->scene_tree->node"
        "xwayland_view_clear_focus_and_grabs(view);"
        "wl_signal_add(&xsurface->events.request_move, &view->xwayland_request_move);"
        "wl_signal_add(&xsurface->events.request_resize, &view->xwayland_request_resize);"
        "if (is_special && !was_special)"
        "xwayland_view_save_geometry(view);"
        "view->saved_geometry_valid = false;")
    string(FIND "${xwayland_source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "missing Xwayland runtime contract: ${required}")
    endif()
endforeach()

string(FIND "${pointer_source}" "wd_xwayland_view_configure_position(view);" configure_position)
string(FIND "${pointer_source}" "wd_scene_set_view_position(view);" scene_position)
if(configure_position EQUAL -1 OR scene_position EQUAL -1 OR configure_position GREATER scene_position)
    message(FATAL_ERROR "interactive Xwayland moves must configure X11 geometry before publishing the scene position")
endif()
