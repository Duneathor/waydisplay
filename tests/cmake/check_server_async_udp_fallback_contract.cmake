if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_async_udp.c" source)

function(require_present text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${pattern}'")
    endif()
endfunction()

function(extract_between text start_marker end_marker out_var)
    string(FIND "${text}" "${start_marker}" start_pos)
    string(FIND "${text}" "${end_marker}" end_pos)
    if(start_pos EQUAL -1 OR end_pos EQUAL -1 OR end_pos LESS_EQUAL start_pos)
        message(FATAL_ERROR "could not isolate block from '${start_marker}' to '${end_marker}'")
    endif()
    math(EXPR block_len "${end_pos} - ${start_pos}")
    string(SUBSTRING "${text}" ${start_pos} ${block_len} block)
    set(${out_var} "${block}" PARENT_SCOPE)
endfunction()

extract_between("${source}"
                "bool wd_async_udp_sender_flush"
                "static enum wd_async_udp_send_status"
                flush_block)
require_present("${flush_block}" "wd_io_uring_submit_result"
                "server UDP must classify submit progress")
require_present("${flush_block}" "WD_IO_URING_SUBMIT_FAILED"
                "permanent submit errors need a separate backend-failure path")
require_present("${flush_block}" "wd_async_udp_retire_ring_to_syscall_fallback"
                "permanent submit errors must retire the ring")

extract_between("${source}"
                "static bool wd_async_udp_progress_syscall_fallback"
                "bool wd_async_udp_sender_flush"
                fallback_block)
require_present("${fallback_block}" "sendmsg(packet->fd"
                "fallback must send whole UDP datagrams with sendmsg")
require_present("${fallback_block}" "MSG_DONTWAIT"
                "UDP syscall fallback must remain nonblocking")
require_present("${fallback_block}" "errno == EINTR || errno == EAGAIN"
                "retryable syscall pressure must retain the queued datagram")

message(STATUS "WayDisplay server async UDP fallback contract verified")
