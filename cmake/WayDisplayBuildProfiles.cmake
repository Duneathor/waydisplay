# GCC-only build profiles for WayDisplay.
#
# Debug   - full debug logging, debug symbols, and no optimization.
# Release - portable optimized build with GCC LTO.
# Profile - instrumented native build used to collect GCC PGO data.
# Native  - native optimized build; consumes PGO data when it exists.

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "WayDisplay supports GCC only. Configure with "
        "-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++.")
endif()

if(NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL CMAKE_CXX_COMPILER_VERSION)
    message(FATAL_ERROR
        "WayDisplay requires matching GCC and G++ versions; found "
        "C ${CMAKE_C_COMPILER_VERSION} and C++ ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

set(WAYDISPLAY_BUILD_PROFILES Debug Release Profile Native)

if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Profile Native)
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING
        "Available WayDisplay build profiles" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING
            "WayDisplay build profile" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        ${WAYDISPLAY_BUILD_PROFILES})

    list(FIND WAYDISPLAY_BUILD_PROFILES "${CMAKE_BUILD_TYPE}"
        _waydisplay_build_profile_index)
    if(_waydisplay_build_profile_index EQUAL -1)
        message(FATAL_ERROR
            "Unsupported CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. "
            "Supported GCC profiles: Debug, Release, Profile, Native.")
    endif()
endif()

set(WAYDISPLAY_PGO_DATA_DIR
    "${CMAKE_SOURCE_DIR}/build-pgo-data"
    CACHE PATH
    "Directory shared by the GCC Profile and Native build profiles")
get_filename_component(WAYDISPLAY_PGO_DATA_DIR
    "${WAYDISPLAY_PGO_DATA_DIR}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Profile")
    file(MAKE_DIRECTORY "${WAYDISPLAY_PGO_DATA_DIR}")
endif()

if(CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 10)
    set(WAYDISPLAY_GCC_LTO_FLAG -flto=auto)
else()
    set(WAYDISPLAY_GCC_LTO_FLAG -flto)
endif()

file(GLOB_RECURSE WAYDISPLAY_PGO_PROFILE_FILES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${WAYDISPLAY_PGO_DATA_DIR}/*.gcda")
if(WAYDISPLAY_PGO_PROFILE_FILES)
    set(WAYDISPLAY_PGO_DATA_AVAILABLE TRUE)
else()
    set(WAYDISPLAY_PGO_DATA_AVAILABLE FALSE)
endif()

# CMake already supplies -g for Debug and -O3/-DNDEBUG for Release. The
# explicit flags below make the WayDisplay profile contract visible and also
# define complete flags for the custom Profile and Native configurations.
add_compile_options(
    "$<$<CONFIG:Debug>:-O0>"
    "$<$<CONFIG:Debug>:-g3>"
    "$<$<CONFIG:Debug>:-ggdb>"
    "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"

    "$<$<CONFIG:Release>:-O3>"
    "$<$<CONFIG:Release>:${WAYDISPLAY_GCC_LTO_FLAG}>"

    "$<$<CONFIG:Profile>:-O3>"
    "$<$<CONFIG:Profile>:${WAYDISPLAY_GCC_LTO_FLAG}>"
    "$<$<CONFIG:Profile>:-march=native>"
    "$<$<CONFIG:Profile>:-mtune=native>"
    "$<$<CONFIG:Profile>:-fprofile-generate=${WAYDISPLAY_PGO_DATA_DIR}>"
    "$<$<CONFIG:Profile>:-fprofile-update=atomic>"

    "$<$<CONFIG:Native>:-O3>"
    "$<$<CONFIG:Native>:${WAYDISPLAY_GCC_LTO_FLAG}>"
    "$<$<CONFIG:Native>:-march=native>"
    "$<$<CONFIG:Native>:-mtune=native>"
)

add_link_options(
    "$<$<CONFIG:Release>:${WAYDISPLAY_GCC_LTO_FLAG}>"

    "$<$<CONFIG:Profile>:${WAYDISPLAY_GCC_LTO_FLAG}>"
    "$<$<CONFIG:Profile>:-fprofile-generate=${WAYDISPLAY_PGO_DATA_DIR}>"

    "$<$<CONFIG:Native>:${WAYDISPLAY_GCC_LTO_FLAG}>"
)

add_compile_definitions(
    "$<$<OR:$<CONFIG:Release>,$<CONFIG:Profile>,$<CONFIG:Native>>:NDEBUG>"
)

if(WAYDISPLAY_PGO_DATA_AVAILABLE)
    add_compile_options(
        "$<$<CONFIG:Native>:-fprofile-use=${WAYDISPLAY_PGO_DATA_DIR}>"
        "$<$<CONFIG:Native>:-fprofile-correction>"
        "$<$<CONFIG:Native>:-Wno-missing-profile>"
    )
    add_link_options(
        "$<$<CONFIG:Native>:-fprofile-use=${WAYDISPLAY_PGO_DATA_DIR}>"
    )
endif()

add_custom_target(pgo-clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${WAYDISPLAY_PGO_DATA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WAYDISPLAY_PGO_DATA_DIR}"
    COMMENT "Removing GCC profile data from ${WAYDISPLAY_PGO_DATA_DIR}"
    VERBATIM
)
