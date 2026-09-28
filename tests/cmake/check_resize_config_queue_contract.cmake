if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_server_net.c" server_source)
string(FIND "${server_source}" "bool wd_server_send_current_config_locked" function_begin)
string(FIND "${server_source}" "static void wd_server_handle_keyboard_message" function_end)
if(function_begin EQUAL -1 OR function_end EQUAL -1 OR function_end LESS_EQUAL function_begin)
    message(FATAL_ERROR "unable to locate wd_server_send_current_config_locked")
endif()
math(EXPR function_length "${function_end} - ${function_begin}")
string(SUBSTRING "${server_source}" ${function_begin} ${function_length} function_body)

string(FIND "${function_body}"
       "wd_async_tcp_sender_drop_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY)"
       drop_pos)
if(drop_pos EQUAL -1)
    message(FATAL_ERROR "resize config path must still discard stale unsubmitted generation summaries")
endif()

string(FIND "${function_body}"
       "wd_async_tcp_sender_has_message_type(net->control_tx, WD_MSG_TILE_GENERATION_SUMMARY)"
       fatal_submitted_summary_pos)
if(NOT fatal_submitted_summary_pos EQUAL -1)
    message(FATAL_ERROR "an already-submitted generation summary must not make a resize config fatal")
endif()

message(STATUS "WayDisplay resize config queue contract verified")
