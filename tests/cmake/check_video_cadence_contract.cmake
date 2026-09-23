if(NOT WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR required")
endif()
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_stream_telemetry.c" telemetry)
foreach(term IN ITEMS
        "capture_target_fps=%u"
        "encoder_nominal_fps=%u"
        "requested_session_fps=%u"
        "capture_pacing_fps=%u"
        "decode_input_drops=%llu"
        "present_queue_replaced=%llu"
        "capture_down=%llu capture_up=%llu")
    string(FIND "${telemetry}" "${term}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "video cadence metric missing: ${term}")
    endif()
endforeach()
string(FIND "${telemetry}" "configured_target_fps=" old_name)
if(NOT old_name EQUAL -1)
    message(FATAL_ERROR "ambiguous legacy configured_target_fps label remains")
endif()
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_stream_video.c" snapshot_path)
string(FIND "${snapshot_path}" "config.target_fps             = wd_video_encoder_nominal_fps(net->stream_policy.requested_session_fps);" stable_clock)
if(stable_clock EQUAL -1)
    message(FATAL_ERROR "encoder clock must use requested session FPS, not adaptive capture FPS")
endif()
message(STATUS "video cadence naming contract passed")
