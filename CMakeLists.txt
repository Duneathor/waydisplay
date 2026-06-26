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

set(CMAKE_CXX_STANDARD 17)
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
