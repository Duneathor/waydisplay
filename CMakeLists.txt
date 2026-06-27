cmake_minimum_required(VERSION 3.20)

project(WayDisplay
    VERSION 0.1.0
    DESCRIPTION "Low-latency remote Wayland display transport"
    LANGUAGES C CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "WayDisplay supports Linux only")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/WayDisplayBuildProfiles.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(WayDisplayOptions)
include(WayDisplayWarnings)
include(WayDisplayDependencies)
include(WayDisplayTargets)
include(WayDisplayTests)
include(WayDisplayInstall)
include(WayDisplaySummary)

# Coverage-guided fuzz targets are intentionally opt-in because libFuzzer is
# compiler-specific and these binaries are not normal CTest cases.
if(WAYDISPLAY_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "WAYDISPLAY_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()
    add_executable(waydisplay_fuzz_tile_protocol tests/fuzz_tile_protocol.cpp)
    target_link_libraries(waydisplay_fuzz_tile_protocol PRIVATE waydisplay_common)
    target_compile_options(waydisplay_fuzz_tile_protocol PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(waydisplay_fuzz_tile_protocol PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(waydisplay_fuzz_tile_reassembly tests/fuzz_tile_reassembly.cpp)
    target_link_libraries(waydisplay_fuzz_tile_reassembly PRIVATE waydisplay_client_tile_reassembly)
    target_include_directories(waydisplay_fuzz_tile_reassembly PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/client)
    target_compile_options(waydisplay_fuzz_tile_reassembly PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(waydisplay_fuzz_tile_reassembly PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
