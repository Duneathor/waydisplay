if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/client/client_async_tcp.cpp" source)

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
                "uint64_t replaceable_tail_pointer_motion_bytes_locked"
                "bool bind_socket_locked"
                coalesce_block)
require_present("${coalesce_block}" "sender->tail"
                "pointer-motion coalescing must work from the queue tail")
require_present("${coalesce_block}" "msg = msg->prev"
                "capacity accounting must walk backward without crossing barriers")
require_present("${coalesce_block}" "wd_async_tcp_message_is_replaceable"
                "started TCP frames must not be coalesced")
require_present("${coalesce_block}" "break;"
                "coalescing must stop at the first ordering barrier")

extract_between("${source}"
                "bool drain_locked"
                "void fail_all_after_ring_exit_locked"
                drain_block)
require_present("${drain_block}" "shutdown_pending_fds_locked(sender)"
                "fallback teardown must shutdown the socket before releasing frames")

message(STATUS "WayDisplay client async TCP coalescing contract verified")
