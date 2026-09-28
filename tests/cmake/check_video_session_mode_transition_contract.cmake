if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/client/client_net.cpp" client_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_stream.c" server_source)

string(FIND "${client_source}" "wd_client_video_offer_decide" client_offer_pos)
if(client_offer_pos EQUAL -1)
    message(FATAL_ERROR "client hello must use the shared effective video-offer policy")
endif()

string(FIND "${server_source}" "wd_video_session_bootstrap_required" server_bootstrap_pos)
if(server_bootstrap_pos EQUAL -1)
    message(FATAL_ERROR "server session begin must gate video bootstrap on negotiated video capability")
endif()

foreach(field IN ITEMS
        video_feedback_last_frame_id_rx
        video_feedback_last_frame_id_decoded
        video_feedback_last_frame_id_presented
        video_feedback_decode_queue_depth
        video_feedback_decode_queue_capacity
        video_feedback_present_queue_depth
        video_feedback_present_queue_capacity
        video_feedback_presentation_stall_ms
        video_feedback_audio_sync_hold_ms
        video_feedback_decoder_phase)
    string(FIND "${server_source}" "policy->${field}" field_pos)
    if(field_pos EQUAL -1)
        message(FATAL_ERROR "server reconnect must reset stale video feedback field ${field}")
    endif()
endforeach()

message(STATUS "WayDisplay video session mode transition contract verified")
