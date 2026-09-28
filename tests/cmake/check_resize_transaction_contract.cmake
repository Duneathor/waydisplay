if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_server.c" server_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_wlroots_backend.c" backend_source)

string(FIND "${server_source}" "bool wd_server_apply_display_size" resize_begin)
string(FIND "${server_source}" "bool wd_server_apply_display_mode" resize_end)
if(resize_begin EQUAL -1 OR resize_end EQUAL -1 OR resize_end LESS_EQUAL resize_begin)
    message(FATAL_ERROR "could not isolate wd_server_apply_display_size")
endif()

math(EXPR resize_length "${resize_end} - ${resize_begin}")
string(SUBSTRING "${server_source}" ${resize_begin} ${resize_length} resize_body)

foreach(required
        "wd_server_compute_geometry(server, width, height, &next_geometry)"
        "wd_resize_allocations_prepare(&next_allocs, &next_geometry)"
        "wd_wlroots_resize_headless_output_to(server, width, height)"
        "pthread_mutex_lock(&server->net.lock)"
        "wd_server_apply_geometry_snapshot(server, &next_geometry)")
    string(FIND "${resize_body}" "${required}" required_pos)
    if(required_pos EQUAL -1)
        message(FATAL_ERROR "resize transaction is missing required step: ${required}")
    endif()
endforeach()

string(FIND "${resize_body}" "wd_server_set_geometry(server, width, height)" premature_publish)
if(NOT premature_publish EQUAL -1)
    message(FATAL_ERROR "live geometry must not be published before the resize transaction owns stream state")
endif()

string(FIND "${resize_body}" "wd_server_compute_geometry(server, width, height, &next_geometry)" compute_pos)
string(FIND "${resize_body}" "wd_resize_allocations_prepare(&next_allocs, &next_geometry)" alloc_pos)
string(FIND "${resize_body}" "wd_wlroots_resize_headless_output_to(server, width, height)" output_pos)
string(FIND "${resize_body}" "pthread_mutex_lock(&server->net.lock)" lock_pos)
string(FIND "${resize_body}" "wd_server_apply_geometry_snapshot(server, &next_geometry)" publish_pos)

if(NOT (compute_pos LESS alloc_pos AND alloc_pos LESS output_pos AND output_pos LESS lock_pos AND lock_pos LESS publish_pos))
    message(FATAL_ERROR "resize geometry/allocation publication order regressed")
endif()

string(FIND "${backend_source}" "bool wd_wlroots_resize_headless_output_to(struct wd_server* server, uint32_t width, uint32_t height)" explicit_mode)
if(explicit_mode EQUAL -1)
    message(FATAL_ERROR "wlroots resize must accept explicit dimensions without reading unpublished server geometry")
endif()

message(STATUS "WayDisplay resize transaction contract verified")
