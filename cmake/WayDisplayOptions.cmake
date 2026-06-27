option(WAYDISPLAY_BUILD_CLIENT_SDL "Build the SDL WayDisplay client" ON)
option(WAYDISPLAY_BUILD_WLROOTS_SERVER "Build the wlroots headless compositor server" ON)
option(WAYDISPLAY_BUILD_TESTS "Build WayDisplay unit tests" ON)
option(WAYDISPLAY_BUILD_FUZZERS "Build coverage-guided libFuzzer targets" OFF)
option(WAYDISPLAY_REQUIRE_RUNTIME_TARGETS "Fail tests when requested SDL/wlroots runtime targets are unavailable" OFF)
option(WAYDISPLAY_RUN_TESTS_ON_BUILD "Run CTest as part of the default build" ON)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_waydisplay_default_log_level DEBUG)
else()
    set(_waydisplay_default_log_level INFO)
endif()
if(DEFINED WAYDISPLAY_LOG_LEVEL)
    string(TOUPPER "${WAYDISPLAY_LOG_LEVEL}" _waydisplay_log_level)
else()
    set(_waydisplay_log_level "${_waydisplay_default_log_level}")
endif()
set(_waydisplay_log_levels OFF ERROR WARN INFO STATS DEBUG)
list(FIND _waydisplay_log_levels "${_waydisplay_log_level}"
    _waydisplay_log_level_index)
if(_waydisplay_log_level_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported WAYDISPLAY_LOG_LEVEL='${WAYDISPLAY_LOG_LEVEL}'. "
        "Supported levels: OFF, ERROR, WARN, INFO, STATS, DEBUG.")
endif()
set(WAYDISPLAY_LOG_LEVEL "${_waydisplay_log_level}" CACHE STRING
    "Highest WayDisplay log level compiled into the binaries" FORCE)
set_property(CACHE WAYDISPLAY_LOG_LEVEL PROPERTY STRINGS
    ${_waydisplay_log_levels})

math(EXPR WAYDISPLAY_LOG_LEVEL_VALUE
    "${_waydisplay_log_level_index} - 1")
option(WAYDISPLAY_ENABLE_XWAYLAND "Enable Xwayland support in the wlroots server" ON)
option(WAYDISPLAY_ENABLE_H265_SERVER_ENCODER "Enable H.265 server encoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H265_CLIENT_DECODER "Enable H.265 client decoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H264_SERVER_ENCODER "Enable H.264 server encoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H264_CLIENT_DECODER "Enable H.264 client decoder backends when available" ON)
option(WAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER "Enable optional VAAPI client hardware decode when available" ON)
option(WAYDISPLAY_ENABLE_AUDIO "Enable audio capture and playback when dependencies are available" ON)
option(WAYDISPLAY_REQUIRE_CODEC_TESTS "Fail configuration unless requested codec tests can be built" OFF)
option(WAYDISPLAY_REQUIRE_WLROOTS_TESTS "Fail configuration unless wlroots tests can be built" OFF)

add_compile_definitions(
    WAYDISPLAY_LOG_LEVEL=${WAYDISPLAY_LOG_LEVEL_VALUE}
)
