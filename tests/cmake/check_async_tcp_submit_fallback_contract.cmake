if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_async_tcp.c" server_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/client/client_async_tcp.cpp" client_source)

function(require_present text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${pattern}'")
    endif()
endfunction()

function(require_absent text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found '${pattern}'")
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

# A fatal io_uring_submit()/SQ failure is a local backend failure. The server
# must preserve the message for syscall fallback instead of incrementing the
# transport-failure counter that drives socket shutdown in wd_server.c.
extract_between("${server_source}"
                "static bool wd_async_tcp_submit_message"
                "static bool wd_async_tcp_try_start_head"
                server_submit_block)
require_present("${server_submit_block}" "wd_async_tcp_retire_ring(sender)"
                "server submit failure must retire the broken ring")
require_present("${server_submit_block}" "sender->syscall_fallback = true"
                "server submit failure must activate syscall fallback")
require_absent("${server_submit_block}" "wd_async_tcp_record_transport_failure"
               "server submit backend failures must not be transport failures")

extract_between("${server_source}"
                "if (sender->submit_retry_pending)"
                "struct io_uring_cqe* cqe = NULL"
                server_retry_block)
require_present("${server_retry_block}" "sender->syscall_fallback = true"
                "server fatal resubmit must activate syscall fallback")
require_absent("${server_retry_block}" "wd_async_tcp_record_transport_failure"
               "server fatal resubmit must not be a transport failure")

# The client must follow the same boundary. Backend failures may retire the
# ring, but they must not mark the connection fatal or shutdown the socket.
extract_between("${client_source}"
                "bool submit_message_locked"
                "bool try_start_head_locked"
                client_submit_block)
require_present("${client_submit_block}" "retire_ring_locked(sender)"
                "client submit failure must retire the broken ring")
require_present("${client_submit_block}" "sender->syscall_fallback = true"
                "client submit failure must activate syscall fallback")
require_absent("${client_submit_block}" "wd_socket_pin_shutdown"
               "client submit backend failure must not shutdown the socket")
require_absent("${client_submit_block}" "sender->fatal = true"
               "client submit backend failure must not mark the connection fatal")

extract_between("${client_source}"
                "if (sender->submit_retry_pending)"
                "io_uring_cqe* cqe = nullptr"
                client_retry_block)
require_present("${client_retry_block}" "sender->syscall_fallback = true"
                "client fatal resubmit must activate syscall fallback")
require_absent("${client_retry_block}" "wd_socket_pin_shutdown"
               "client fatal resubmit must not shutdown the socket")
require_absent("${client_retry_block}" "sender->fatal = true"
               "client fatal resubmit must not mark the connection fatal")

message(STATUS "WayDisplay async TCP submit fallback contract verified")
