if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE _waydisplay_sources
    "${WAYDISPLAY_SOURCE_DIR}/src/*.c"
    "${WAYDISPLAY_SOURCE_DIR}/src/*.cpp")

set(_allowed_helpers
    io_uring_prep_send
    io_uring_prep_sendmsg
    io_uring_prep_recv
    io_uring_prep_cancel)

foreach(_source IN LISTS _waydisplay_sources)
    file(READ "${_source}" _content)
    string(REGEX MATCHALL "io_uring_prep_[A-Za-z0-9_]+" _helpers "${_content}")
    foreach(_helper IN LISTS _helpers)
        list(FIND _allowed_helpers "${_helper}" _helper_index)
        if(_helper_index EQUAL -1)
            message(FATAL_ERROR
                "${_source} uses ${_helper}, which is outside the Linux 5.14 io_uring contract")
        endif()
    endforeach()
endforeach()
