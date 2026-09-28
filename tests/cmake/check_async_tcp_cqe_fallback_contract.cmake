if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(READ "${WAYDISPLAY_SOURCE_DIR}/src/server/wd_async_tcp.c" server_source)
file(READ "${WAYDISPLAY_SOURCE_DIR}/src/client/client_async_tcp.cpp" client_source)

file(READ "${WAYDISPLAY_SOURCE_DIR}/include/waydisplay/wd_async_tcp_policy.h" policy_source)
string(FIND "${policy_source}" "wd_async_tcp_cqe_should_try_syscall" fallback_policy_pos)
string(FIND "${policy_source}" "-EBADF" ebadf_policy_pos)
if(fallback_policy_pos EQUAL -1 OR ebadf_policy_pos EQUAL -1)
    message(FATAL_ERROR "shared CQE fallback policy must validate EOPNOTSUPP and EBADF through the pinned syscall socket")
endif()

foreach(source_name IN ITEMS server_source client_source)
    string(FIND "${${source_name}}" "wd_async_tcp_cqe_should_try_syscall" fallback_pos)
    if(fallback_pos EQUAL -1)
        message(FATAL_ERROR "${source_name} must use the shared CQE-to-syscall fallback policy")
    endif()

    string(FIND "${${source_name}}" "MSG_DONTWAIT" dontwait_pos)
    if(dontwait_pos EQUAL -1)
        message(FATAL_ERROR "${source_name} fallback must use a nonblocking syscall send")
    endif()
endforeach()

message(STATUS "WayDisplay async TCP CQE fallback contract verified")
