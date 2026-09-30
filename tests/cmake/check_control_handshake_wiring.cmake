if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

set(source "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_server_net.c")
file(READ "${source}" text)

string(FIND "${text}" "static bool wd_receive_client_hello" begin)
if(begin EQUAL -1)
    message(FATAL_ERROR "wd_receive_client_hello not found")
endif()
string(SUBSTRING "${text}" ${begin} -1 tail)
string(FIND "${tail}" "static bool wd_receive_client_negotiation_message" end)
if(end EQUAL -1)
    message(FATAL_ERROR "wd_receive_client_hello end marker not found")
endif()
string(SUBSTRING "${tail}" 0 ${end} body)

foreach(required
        "wd_tcp_reader_init(&reader"
        "wd_wait_server_negotiation_message(server, tcp_fd, &reader, deadline_ns, &message)"
        "WD_TCP_HANDSHAKE_TIMEOUT_MS"
        "wd_tcp_reader_destroy(&reader)")
    string(FIND "${body}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "initial hello path lost bounded-reader contract: ${required}")
    endif()
endforeach()

string(FIND "${body}" "wd_recv_all(" blocking_receive)
if(NOT blocking_receive EQUAL -1)
    message(FATAL_ERROR "initial hello path regressed to blocking wd_recv_all")
endif()
