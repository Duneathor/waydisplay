if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

set(_server_source "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_server_net.c")
file(READ "${_server_source}" _content)

string(FIND "${_content}" "struct wd_tcp_message control_message;" _control_begin)
string(FIND "${_content}" "wd_tcp_reader_destroy(&control_reader);" _control_end)
if(_control_begin EQUAL -1 OR _control_end EQUAL -1 OR _control_end LESS _control_begin)
    message(FATAL_ERROR "could not locate established control-channel reader lifetime")
endif()

math(EXPR _control_length "${_control_end} - ${_control_begin}")
string(SUBSTRING "${_content}" ${_control_begin} ${_control_length} _control_loop)

if(_control_loop MATCHES "free[ \t\r\n]*\\([ \t\r\n]*payload[ \t\r\n]*\\)")
    message(FATAL_ERROR "control reader payload is a wd_buffer view and must not be freed directly")
endif()

string(REGEX MATCHALL "wd_tcp_message_release[ \t\r\n]*\\([ \t\r\n]*&control_message[ \t\r\n]*\\)" _releases "${_control_loop}")
list(LENGTH _releases _release_count)
if(_release_count LESS 4)
    message(FATAL_ERROR "control reader ownership exits must release control_message")
endif()
