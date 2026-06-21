cmake_minimum_required(VERSION 3.20)

project(WayDisplay
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WAYDISPLAY_BUILD_CLIENT_SDL "Build the SDL WayDisplay client" ON)
option(WAYDISPLAY_BUILD_WLROOTS_SERVER "Build the wlroots headless compositor server" ON)
option(WAYDISPLAY_BUILD_LEGACY_PROTO "Build the old monolithic prototype" OFF)
option(WAYDISPLAY_BUILD_TESTS "Build WayDisplay unit tests" ON)
option(WAYDISPLAY_ENABLE_LOGGING "Enable WayDisplay runtime logging" ON)
option(WAYDISPLAY_ENABLE_DEBUG_LOGGING "Enable WayDisplay debug-level logging" OFF)
option(WAYDISPLAY_ENABLE_XWAYLAND "Enable Xwayland support in the wlroots server" ON)
option(WAYDISPLAY_ENABLE_H265_SERVER_ENCODER "Enable H.265 server encoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H265_CLIENT_DECODER "Enable H.265 client decoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H264_SERVER_ENCODER "Enable H.264 server encoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H264_CLIENT_DECODER "Enable H.264 client decoder backends when available" ON)
option(WAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER "Enable optional VAAPI client hardware decode when available" ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)


# -----------------------------------------------------------------------------
# Optional video codec backends
# -----------------------------------------------------------------------------

set(WAYDISPLAY_HAVE_H265_SERVER_ENCODER FALSE)
set(WAYDISPLAY_HAVE_H265_CLIENT_DECODER FALSE)
set(WAYDISPLAY_HAVE_H264_SERVER_ENCODER FALSE)
set(WAYDISPLAY_HAVE_H264_CLIENT_DECODER FALSE)
set(WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER FALSE)

if(WAYDISPLAY_ENABLE_H265_SERVER_ENCODER OR WAYDISPLAY_ENABLE_H264_SERVER_ENCODER)
    pkg_check_modules(FFMPEG_H265_ENCODER QUIET IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale
    )

    if(FFMPEG_H265_ENCODER_FOUND)
        if(WAYDISPLAY_ENABLE_H265_SERVER_ENCODER)
            set(WAYDISPLAY_HAVE_H265_SERVER_ENCODER TRUE)
        endif()
        if(WAYDISPLAY_ENABLE_H264_SERVER_ENCODER)
            set(WAYDISPLAY_HAVE_H264_SERVER_ENCODER TRUE)
        endif()
    else()
        message(STATUS "Video server encoder backends disabled: libavcodec/libavutil/libswscale not found")
    endif()
endif()

if(WAYDISPLAY_ENABLE_H265_CLIENT_DECODER OR WAYDISPLAY_ENABLE_H264_CLIENT_DECODER)
    pkg_check_modules(FFMPEG_H265_DECODER QUIET IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale
    )

    if(FFMPEG_H265_DECODER_FOUND)
        if(WAYDISPLAY_ENABLE_H265_CLIENT_DECODER)
            set(WAYDISPLAY_HAVE_H265_CLIENT_DECODER TRUE)
        endif()
        if(WAYDISPLAY_ENABLE_H264_CLIENT_DECODER)
            set(WAYDISPLAY_HAVE_H264_CLIENT_DECODER TRUE)
        endif()
        if(WAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER)
            set(WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER TRUE)
        endif()
    else()
        message(STATUS "Video client decoder backends disabled: libavcodec/libavutil/libswscale not found")
    endif()
endif()

function(waydisplay_apply_common_warnings target_name)
    target_compile_options(${target_name} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endfunction()

function(waydisplay_find_wlroots)
    set(WAYDISPLAY_WLROOTS_CANDIDATES
        wlroots
        wlroots-0.19
        wlroots0.19
        wlroots-0.20
        wlroots0.20
    )

    foreach(candidate IN LISTS WAYDISPLAY_WLROOTS_CANDIDATES)
        pkg_check_modules(WLROOTS_CANDIDATE QUIET IMPORTED_TARGET ${candidate})

        if(WLROOTS_CANDIDATE_FOUND)
            set(WAYDISPLAY_WLROOTS_MODULE ${candidate} PARENT_SCOPE)
            set(WAYDISPLAY_WLROOTS_FOUND TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(WAYDISPLAY_WLROOTS_MODULE "" PARENT_SCOPE)
    set(WAYDISPLAY_WLROOTS_FOUND FALSE PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# Common library
# -----------------------------------------------------------------------------

pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_library(waydisplay_common STATIC
    src/common/wd_time.c
    src/common/wd_log.c
    src/common/wd_net.c
    src/common/wd_tile.c
    src/common/wd_zstd.c
)

waydisplay_apply_common_warnings(waydisplay_common)

target_include_directories(waydisplay_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(waydisplay_common PUBLIC
    WAYDISPLAY_ENABLE_LOGGING=$<BOOL:${WAYDISPLAY_ENABLE_LOGGING}>
    WAYDISPLAY_ENABLE_DEBUG_LOGGING=$<BOOL:${WAYDISPLAY_ENABLE_DEBUG_LOGGING}>
)

target_link_libraries(waydisplay_common PUBLIC
    PkgConfig::ZSTD
)


add_library(waydisplay_server_tile_policy STATIC
    src/server/wd_tile_policy.c
    src/server/wd_dirty_region_scheduler.c
    src/server/wd_async_udp_accounting.c
    src/server/wd_video_transition.c
    src/server/wd_input_correlation.c
)

waydisplay_apply_common_warnings(waydisplay_server_tile_policy)

target_include_directories(waydisplay_server_tile_policy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(waydisplay_client_config STATIC
    src/client/client_config_validation.cpp
)

waydisplay_apply_common_warnings(waydisplay_client_config)

target_include_directories(waydisplay_client_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(waydisplay_client_config PUBLIC
    waydisplay_common
)

add_library(waydisplay_client_render_planning STATIC
    src/client/render_planning.cpp
    src/client/content_order.cpp
    src/client/present_telemetry.cpp
    src/client/render_wakeup.cpp
    src/client/stream_ownership.cpp
    src/client/udp_transport_lifecycle.cpp
)

waydisplay_apply_common_warnings(waydisplay_client_render_planning)

target_include_directories(waydisplay_client_render_planning PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(waydisplay_client_tile_reassembly STATIC
    src/client/tile_reassembly.cpp
)

waydisplay_apply_common_warnings(waydisplay_client_tile_reassembly)

target_include_directories(waydisplay_client_tile_reassembly PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client
)

target_link_libraries(waydisplay_client_tile_reassembly PUBLIC
    waydisplay_common
    Threads::Threads
)

# -----------------------------------------------------------------------------
# SDL client
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_CLIENT_SDL)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

    add_executable(waydisplay_client_sdl
        src/client/main_sdl.cpp
        src/client/client_net.cpp
        src/client/client_async_tcp.cpp
        src/client/client_async_udp.cpp
        src/client/video_decoder.cpp
        src/client/sdl_input.cpp
        src/client/sdl_viewer.cpp
    )

    waydisplay_apply_common_warnings(waydisplay_client_sdl)

    target_include_directories(waydisplay_client_sdl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    target_compile_definitions(waydisplay_client_sdl PRIVATE
        WAYDISPLAY_HAVE_H265_CLIENT_DECODER=$<BOOL:${WAYDISPLAY_HAVE_H265_CLIENT_DECODER}>
        WAYDISPLAY_HAVE_H264_CLIENT_DECODER=$<BOOL:${WAYDISPLAY_HAVE_H264_CLIENT_DECODER}>
        WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER=$<BOOL:${WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER}>
    )

    target_link_libraries(waydisplay_client_sdl PRIVATE
        waydisplay_common
        waydisplay_client_config
        waydisplay_client_render_planning
        waydisplay_client_tile_reassembly
        PkgConfig::SDL3
        PkgConfig::LIBURING
        Threads::Threads
    )

    if(WAYDISPLAY_HAVE_H265_CLIENT_DECODER OR WAYDISPLAY_HAVE_H264_CLIENT_DECODER)
        target_link_libraries(waydisplay_client_sdl PRIVATE
            PkgConfig::FFMPEG_H265_DECODER
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# wlroots compositor server
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_WLROOTS_SERVER)
    waydisplay_find_wlroots()

    pkg_check_modules(WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
    pkg_check_modules(WAYLAND_PROTOCOLS QUIET IMPORTED_TARGET wayland-protocols)
    pkg_check_modules(XKBCOMMON QUIET IMPORTED_TARGET xkbcommon)
    pkg_check_modules(PIXMAN QUIET IMPORTED_TARGET pixman-1)
    pkg_check_modules(LIBDRM QUIET IMPORTED_TARGET libdrm)
    pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)

    find_program(WAYLAND_SCANNER wayland-scanner)

    if(WAYDISPLAY_WLROOTS_FOUND
       AND WAYLAND_SERVER_FOUND
       AND WAYLAND_PROTOCOLS_FOUND
       AND XKBCOMMON_FOUND
       AND PIXMAN_FOUND
       AND LIBDRM_FOUND
       AND LIBURING_FOUND
       AND WAYLAND_SCANNER)

        message(STATUS "Using wlroots pkg-config module: ${WAYDISPLAY_WLROOTS_MODULE}")

        pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET ${WAYDISPLAY_WLROOTS_MODULE})

        pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)

        set(XDG_SHELL_XML
            ${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml
        )

        set(XDG_DECORATION_XML
            ${WAYLAND_PROTOCOLS_DATADIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
        )

        set(XDG_TOPLEVEL_ICON_XML
            ${WAYLAND_PROTOCOLS_DATADIR}/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml
        )

        set(CURSOR_SHAPE_XML
            ${WAYLAND_PROTOCOLS_DATADIR}/staging/cursor-shape/cursor-shape-v1.xml
        )

        set(XDG_DIALOG_XML
            ${WAYLAND_PROTOCOLS_DATADIR}/staging/xdg-dialog/xdg-dialog-v1.xml
        )

        set(GENERATED_PROTOCOL_DIR
            ${CMAKE_CURRENT_BINARY_DIR}/generated-protocols
        )

        file(MAKE_DIRECTORY ${GENERATED_PROTOCOL_DIR})

        add_custom_command(
            OUTPUT
                ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.h
                ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.c
            COMMAND ${WAYLAND_SCANNER} server-header
                    ${XDG_SHELL_XML}
                    ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.h
            COMMAND ${WAYLAND_SCANNER} private-code
                    ${XDG_SHELL_XML}
                    ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.c
            DEPENDS ${XDG_SHELL_XML}
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${GENERATED_PROTOCOL_DIR}/xdg-decoration-unstable-v1-protocol.h
            COMMAND ${WAYLAND_SCANNER} server-header
                    ${XDG_DECORATION_XML}
                    ${GENERATED_PROTOCOL_DIR}/xdg-decoration-unstable-v1-protocol.h
            DEPENDS ${XDG_DECORATION_XML}
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${GENERATED_PROTOCOL_DIR}/xdg-toplevel-icon-v1-protocol.h
            COMMAND ${WAYLAND_SCANNER} server-header
                    ${XDG_TOPLEVEL_ICON_XML}
                    ${GENERATED_PROTOCOL_DIR}/xdg-toplevel-icon-v1-protocol.h
            DEPENDS ${XDG_TOPLEVEL_ICON_XML}
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${GENERATED_PROTOCOL_DIR}/cursor-shape-v1-protocol.h
            COMMAND ${WAYLAND_SCANNER} server-header
                    ${CURSOR_SHAPE_XML}
                    ${GENERATED_PROTOCOL_DIR}/cursor-shape-v1-protocol.h
            DEPENDS ${CURSOR_SHAPE_XML}
            VERBATIM
        )

        add_custom_command(
            OUTPUT
                ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.h
                ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.c
            COMMAND ${WAYLAND_SCANNER} server-header
                    ${XDG_DIALOG_XML}
                    ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.h
            COMMAND ${WAYLAND_SCANNER} private-code
                    ${XDG_DIALOG_XML}
                    ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.c
            DEPENDS ${XDG_DIALOG_XML}
            VERBATIM
        )

        add_custom_target(waydisplay_wayland_protocols
            DEPENDS
                ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.h
                ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.c
                ${GENERATED_PROTOCOL_DIR}/xdg-decoration-unstable-v1-protocol.h
                ${GENERATED_PROTOCOL_DIR}/xdg-toplevel-icon-v1-protocol.h
                ${GENERATED_PROTOCOL_DIR}/cursor-shape-v1-protocol.h
                ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.h
                ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.c
        )

        add_executable(waydisplay_server_wlroots
            ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.c
            ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.c
            src/server/main_wlroots.c
            src/server/wd_server.c
            src/server/wd_server_net.c
            src/server/wd_async_tcp.c
            src/server/wd_async_udp.c
            src/server/wd_stream.c
            src/server/wd_video_encoder.c
            src/server/wd_keyboard.c
            src/server/wd_keyboard_shortcuts_inhibit.c
            src/server/wd_clipboard.c
            src/server/wd_cursor.c
            src/server/wd_xdg_activation.c
            src/server/wd_xdg_foreign.c
            src/server/wd_xdg_dialog.c
            src/server/wd_xdg_toplevel_icon.c
            src/server/wd_xdg_decoration.c
            src/server/wd_wlroots_backend.c
            src/server/wd_xwayland.c
            src/server/wd_scene.c
            src/server/wd_readback.c
            src/server/wd_pointer.c
        )

        add_dependencies(waydisplay_server_wlroots
            waydisplay_wayland_protocols
        )

        waydisplay_apply_common_warnings(waydisplay_server_wlroots)

        target_compile_definitions(waydisplay_server_wlroots PRIVATE
            WLR_USE_UNSTABLE
            WAYDISPLAY_ENABLE_XWAYLAND=$<BOOL:${WAYDISPLAY_ENABLE_XWAYLAND}>
            WAYDISPLAY_ENABLE_H265_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_ENABLE_H265_SERVER_ENCODER}>
            WAYDISPLAY_HAVE_H265_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_HAVE_H265_SERVER_ENCODER}>
            WAYDISPLAY_HAVE_H264_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_HAVE_H264_SERVER_ENCODER}>
        )

        target_include_directories(waydisplay_server_wlroots PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
            ${GENERATED_PROTOCOL_DIR}
        )

        target_link_libraries(waydisplay_server_wlroots PRIVATE
            waydisplay_common
            waydisplay_server_tile_policy
            PkgConfig::WLROOTS
            PkgConfig::WAYLAND_SERVER
            PkgConfig::XKBCOMMON
            PkgConfig::PIXMAN
            PkgConfig::LIBDRM
            PkgConfig::LIBURING
            Threads::Threads
        )

        if(WAYDISPLAY_HAVE_H265_SERVER_ENCODER OR WAYDISPLAY_HAVE_H264_SERVER_ENCODER)
            target_link_libraries(waydisplay_server_wlroots PRIVATE
                PkgConfig::FFMPEG_H265_ENCODER
            )
        endif()
    else()
        message(WARNING
            "Skipping waydisplay_server_wlroots because one or more dependencies were not found.\n"
            "Tried wlroots pkg-config modules: wlroots, wlroots-0.19, wlroots0.19, wlroots-0.18, wlroots0.18\n"
            "Required pkg-config modules: wayland-server, wayland-protocols, xkbcommon, pixman-1, libdrm, liburing\n"
            "Required executable: wayland-scanner\n"
            "Found status:\n"
            "  wlroots:           ${WAYDISPLAY_WLROOTS_FOUND}\n"
            "  wlroots module:    ${WAYDISPLAY_WLROOTS_MODULE}\n"
            "  wayland-server:    ${WAYLAND_SERVER_FOUND}\n"
            "  wayland-protocols: ${WAYLAND_PROTOCOLS_FOUND}\n"
            "  xkbcommon:         ${XKBCOMMON_FOUND}\n"
            "  pixman-1:          ${PIXMAN_FOUND}\n"
            "  libdrm:            ${LIBDRM_FOUND}\n"
            "  liburing:          ${LIBURING_FOUND}\n"
            "  wayland-scanner:   ${WAYLAND_SCANNER}\n"
            "Set -DWAYDISPLAY_BUILD_WLROOTS_SERVER=OFF to silence this warning."
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# Legacy monolithic prototype
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_LEGACY_PROTO)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    add_executable(waydisplay_proto
        waydisplay_proto.cpp
    )

    waydisplay_apply_common_warnings(waydisplay_proto)

    target_link_libraries(waydisplay_proto PRIVATE
        waydisplay_common
        PkgConfig::SDL2
        Threads::Threads
    )
endif()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_TESTS)
    include(CTest)

    add_executable(waydisplay_test_client_config
        tests/test_client_config.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_client_config)
    target_link_libraries(waydisplay_test_client_config PRIVATE waydisplay_client_config)
    add_test(NAME waydisplay.client_config COMMAND waydisplay_test_client_config)

    add_executable(waydisplay_test_tile_protocol
        tests/test_tile_protocol.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_tile_protocol)
    target_link_libraries(waydisplay_test_tile_protocol PRIVATE waydisplay_common)
    add_test(NAME waydisplay.tile_protocol COMMAND waydisplay_test_tile_protocol)

    add_executable(waydisplay_test_tile_reassembly
        tests/test_tile_reassembly.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_tile_reassembly)
    target_link_libraries(waydisplay_test_tile_reassembly PRIVATE waydisplay_client_tile_reassembly)
    add_test(NAME waydisplay.tile_reassembly COMMAND waydisplay_test_tile_reassembly)

    add_executable(waydisplay_test_server_tile_policy
        tests/test_server_tile_policy.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_server_tile_policy)
    target_link_libraries(waydisplay_test_server_tile_policy PRIVATE waydisplay_server_tile_policy)
    add_test(NAME waydisplay.server_tile_policy COMMAND waydisplay_test_server_tile_policy)

    add_executable(waydisplay_test_dirty_region_scheduler
        tests/test_dirty_region_scheduler.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_dirty_region_scheduler)
    target_link_libraries(waydisplay_test_dirty_region_scheduler PRIVATE waydisplay_server_tile_policy)
    add_test(NAME waydisplay.dirty_region_scheduler COMMAND waydisplay_test_dirty_region_scheduler)

    add_executable(waydisplay_test_async_udp_accounting
        tests/test_async_udp_accounting.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_async_udp_accounting)
    target_link_libraries(waydisplay_test_async_udp_accounting PRIVATE waydisplay_server_tile_policy)
    add_test(NAME waydisplay.async_udp_accounting COMMAND waydisplay_test_async_udp_accounting)

    add_executable(waydisplay_test_transition_integrity
        tests/test_transition_integrity.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_transition_integrity)
    target_link_libraries(waydisplay_test_transition_integrity PRIVATE
        waydisplay_server_tile_policy
        waydisplay_client_render_planning
    )
    add_test(NAME waydisplay.transition_integrity COMMAND waydisplay_test_transition_integrity)

    add_executable(waydisplay_test_render_planning
        tests/test_render_planning.cpp
    )
    waydisplay_apply_common_warnings(waydisplay_test_render_planning)
    target_link_libraries(waydisplay_test_render_planning PRIVATE waydisplay_client_render_planning Threads::Threads)
    add_test(NAME waydisplay.render_planning COMMAND waydisplay_test_render_planning)
endif()

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------

message(STATUS "WayDisplay build options:")
message(STATUS "  WAYDISPLAY_BUILD_CLIENT_SDL=${WAYDISPLAY_BUILD_CLIENT_SDL}")
message(STATUS "  WAYDISPLAY_BUILD_WLROOTS_SERVER=${WAYDISPLAY_BUILD_WLROOTS_SERVER}")
message(STATUS "  WAYDISPLAY_BUILD_LEGACY_PROTO=${WAYDISPLAY_BUILD_LEGACY_PROTO}")
message(STATUS "  WAYDISPLAY_BUILD_TESTS=${WAYDISPLAY_BUILD_TESTS}")
message(STATUS "  WAYDISPLAY_ENABLE_LOGGING=${WAYDISPLAY_ENABLE_LOGGING}")
message(STATUS "  WAYDISPLAY_ENABLE_DEBUG_LOGGING=${WAYDISPLAY_ENABLE_DEBUG_LOGGING}")
message(STATUS "  WAYDISPLAY_ENABLE_XWAYLAND=${WAYDISPLAY_ENABLE_XWAYLAND}")
message(STATUS "  WAYDISPLAY_ENABLE_H265_SERVER_ENCODER=${WAYDISPLAY_ENABLE_H265_SERVER_ENCODER}")
message(STATUS "  WAYDISPLAY_ENABLE_H265_CLIENT_DECODER=${WAYDISPLAY_ENABLE_H265_CLIENT_DECODER}")
message(STATUS "  WAYDISPLAY_ENABLE_H264_SERVER_ENCODER=${WAYDISPLAY_ENABLE_H264_SERVER_ENCODER}")
message(STATUS "  WAYDISPLAY_ENABLE_H264_CLIENT_DECODER=${WAYDISPLAY_ENABLE_H264_CLIENT_DECODER}")
message(STATUS "  WAYDISPLAY_HAVE_H265_SERVER_ENCODER=${WAYDISPLAY_HAVE_H265_SERVER_ENCODER}")
message(STATUS "  WAYDISPLAY_HAVE_H265_CLIENT_DECODER=${WAYDISPLAY_HAVE_H265_CLIENT_DECODER}")
message(STATUS "  WAYDISPLAY_HAVE_H264_SERVER_ENCODER=${WAYDISPLAY_HAVE_H264_SERVER_ENCODER}")
message(STATUS "  WAYDISPLAY_HAVE_H264_CLIENT_DECODER=${WAYDISPLAY_HAVE_H264_CLIENT_DECODER}")

if(DEFINED WAYDISPLAY_WLROOTS_MODULE)
    message(STATUS "  WAYDISPLAY_WLROOTS_MODULE=${WAYDISPLAY_WLROOTS_MODULE}")
endif()
