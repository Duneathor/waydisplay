if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

set(_stream_source "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_stream_video.c")
file(READ "${_stream_source}" _content)

if(NOT _content MATCHES "wd_async_tcp_send_owned_message[ \t\r\n]*\\(")
    message(FATAL_ERROR "video stream must queue encoded bytes through the owned TCP payload path")
endif()

if(_content MATCHES "wd_async_tcp_prepare_message[ \t\r\n]*\\([ \t\r\n]*WD_MSG_VIDEO_FRAME")
    message(FATAL_ERROR "video stream regressed to allocating a contiguous TCP video payload")
endif()

if(_content MATCHES "memcpy[ \t\r\n]*\\([^;]*packet\\.data")
    message(FATAL_ERROR "video stream regressed to copying encoded packet bytes before async send")
endif()
