if(NOT DEFINED WAYDISPLAY_TEST_FILE)
    message(FATAL_ERROR "WAYDISPLAY_TEST_FILE is required")
endif()
if(NOT EXISTS "${WAYDISPLAY_TEST_FILE}")
    message(FATAL_ERROR "CTest file does not exist: ${WAYDISPLAY_TEST_FILE}")
endif()

file(READ "${WAYDISPLAY_TEST_FILE}" _waydisplay_ctest)

foreach(_suffix IN ITEMS
        VIDEO_ENCODER
        VIDEO_DECODER
        VIDEO_ENCODER_VAAPI
        VIDEO_DECODER_VAAPI
        VIDEO_RESIZE_ROUNDTRIP
        VIDEO_CODEC_ROUNDTRIP)
    string(TOLOWER "${_suffix}" _test_suffix)
    set(_test_name "waydisplay.${_test_suffix}")
    string(FIND "${_waydisplay_ctest}" "${_test_name}" _index)
    if(WAYDISPLAY_EXPECT_${_suffix})
        if(_index EQUAL -1)
            message(FATAL_ERROR "Expected codec test is not registered: ${_test_name}")
        endif()
    else()
        if(NOT _index EQUAL -1)
            message(FATAL_ERROR "Codec test registered without its backend: ${_test_name}")
        endif()
    endif()
endforeach()
