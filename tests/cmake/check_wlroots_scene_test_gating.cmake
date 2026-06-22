if(NOT DEFINED WAYDISPLAY_TEST_FILE)
    message(FATAL_ERROR "WAYDISPLAY_TEST_FILE is required")
endif()
if(NOT EXISTS "${WAYDISPLAY_TEST_FILE}")
    message(FATAL_ERROR "CTest file does not exist: ${WAYDISPLAY_TEST_FILE}")
endif()

file(READ "${WAYDISPLAY_TEST_FILE}" _waydisplay_ctest)

foreach(_name IN ITEMS
        waydisplay.wlroots_scene_graph
        waydisplay.wlroots_scene_lifecycle)
    string(FIND "${_waydisplay_ctest}" "${_name}" _index)
    if(WAYDISPLAY_EXPECT_WLROOTS_TESTS)
        if(_index EQUAL -1)
            message(FATAL_ERROR "Expected wlroots test is not registered: ${_name}")
        endif()
    else()
        if(NOT _index EQUAL -1)
            message(FATAL_ERROR "wlroots test registered without wlroots server: ${_name}")
        endif()
    endif()
endforeach()
