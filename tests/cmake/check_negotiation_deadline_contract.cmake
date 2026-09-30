if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_server_net.c" server_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/client/client_net.cpp" client_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/client/client_transport.cpp" client_transport_source)

function(require_present text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${pattern}'")
    endif()
endfunction()

function(require_absent text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found forbidden '${pattern}'")
    endif()
endfunction()

function(extract_between text start_marker end_marker out_var)
    string(FIND "${text}" "${start_marker}" start_pos)
    if(start_pos EQUAL -1)
        message(FATAL_ERROR "missing start marker '${start_marker}'")
    endif()
    string(SUBSTRING "${text}" ${start_pos} -1 tail)
    string(FIND "${tail}" "${end_marker}" relative_end)
    if(relative_end EQUAL -1 OR relative_end EQUAL 0)
        message(FATAL_ERROR "could not isolate block from '${start_marker}' to '${end_marker}'")
    endif()
    string(SUBSTRING "${tail}" 0 ${relative_end} block)
    set(${out_var} "${block}" PARENT_SCOPE)
endfunction()

extract_between("${server_source}"
                "static bool wd_receive_client_hello"
                "static uint64_t wd_clamp_u64"
                server_hello_block)
require_present("${server_hello_block}" "wd_wait_server_negotiation_message"
                "initial client hello must use the bounded incremental reader")
require_absent("${server_hello_block}" "wd_recv_all"
               "initial client hello must not use blocking exact-size reads")

extract_between("${server_source}"
                "static uint16_t run_udp_mtu_probe"
                "static int wd_tcp_reader_poll_timeout_ms"
                server_negotiation_block)
require_present("${server_negotiation_block}" "wd_receive_client_negotiation_message"
                "server probe replies must use bounded negotiation reads")
require_present("${server_negotiation_block}" "wd_server_negotiation_active"
                "server probe loops must honor shutdown and the absolute negotiation deadline")
require_absent("${server_negotiation_block}" "wd_recv_tcp_message"
               "pre-session server negotiation must not use blocking message reads")

extract_between("${server_source}"
                "static bool wd_accept_aux_channel_fd"
                "static bool wd_accept_required_aux_channels"
                aux_block)
require_present("${aux_block}" "wd_wait_server_negotiation_message"
                "auxiliary channel hello must use the bounded incremental reader")
require_present("${aux_block}" "absolute_deadline_ns"
                "auxiliary channel hello must be bounded by an absolute deadline")

extract_between("${client_transport_source}"
                "int connect_tcp_fd"
                "bool open_tcp_socket"
                client_connect_block)
require_present("${client_connect_block}" "SOCK_NONBLOCK"
                "client TCP connect must not block past the configured handshake deadline")
require_present("${client_connect_block}" "WD_TCP_HANDSHAKE_TIMEOUT_MS"
                "client TCP connect must use the configured handshake timeout")
require_present("${client_connect_block}" "poll"
                "client TCP connect must wait with a bounded poll")

extract_between("${client_source}"
                "bool receive_server_config"
                "struct SummaryRepairCandidate"
                client_negotiation_block)
require_present("${client_negotiation_block}" "wd_tcp_reader_wait_for_message"
                "client negotiation must use the bounded incremental reader")
require_present("${client_negotiation_block}" "WD_TCP_NEGOTIATION_TIMEOUT_MS"
                "client negotiation must have an overall deadline")
require_absent("${client_negotiation_block}" "wd_recv_tcp_message"
               "client negotiation must not use blocking message reads")

message(STATUS "WayDisplay bounded negotiation contract verified")
