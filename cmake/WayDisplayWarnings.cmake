option(WAYDISPLAY_STRICT_WARNINGS "Enable WayDisplay's strict warning baseline" ON)
option(WAYDISPLAY_WARNINGS_AS_ERRORS "Treat WayDisplay warnings as errors" OFF)

function(waydisplay_apply_common_warnings target_name)
    target_compile_options(${target_name} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wformat=2
        -Wundef
        -Wshadow
        -Wcast-align
        -Wwrite-strings
        -Wnull-dereference
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
    )

    if(WAYDISPLAY_STRICT_WARNINGS)
        target_compile_options(${target_name} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
            $<$<COMPILE_LANGUAGE:C>:-Wold-style-definition>
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
            $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        )
    endif()

    if(WAYDISPLAY_WARNINGS_AS_ERRORS)
        target_compile_options(${target_name} PRIVATE -Werror)
    endif()
endfunction()
