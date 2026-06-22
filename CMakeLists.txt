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
option(WAYDISPLAY_RUN_TESTS_ON_BUILD "Run CTest as part of the default build" ON)
option(WAYDISPLAY_BUILD_FUZZERS "Build libFuzzer protocol/reassembly targets" OFF)
option(WAYDISPLAY_ENABLE_LOGGING "Enable WayDisplay runtime logging" ON)
option(WAYDISPLAY_ENABLE_DEBUG_LOGGING "Enable WayDisplay debug-level logging" OFF)
option(WAYDISPLAY_ENABLE_XWAYLAND "Enable Xwayland support in the wlroots server" ON)
option(WAYDISPLAY_ENABLE_H265_SERVER_ENCODER "Enable H.265 server encoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H265_CLIENT_DECODER "Enable H.265 client decoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H264_SERVER_ENCODER "Enable H.264 server encoder backends when available" ON)
option(WAYDISPLAY_ENABLE_H264_CLIENT_DECODER "Enable H.264 client decoder backends when available" ON)
option(WAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER "Enable optional VAAPI client hardware decode when available" ON)
option(WAYDISPLAY_ENABLE_AUDIO "Enable audio capture and playback when dependencies are available" ON)
option(WAYDISPLAY_REQUIRE_CODEC_TESTS "Fail configuration unless requested codec tests can be built" OFF)
option(WAYDISPLAY_REQUIRE_WLROOTS_TESTS "Fail configuration unless wlroots tests can be built" OFF)

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
set(WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE FALSE)
set(WAYDISPLAY_HAVE_OPUS_AUDIO FALSE)

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

if(WAYDISPLAY_REQUIRE_CODEC_TESTS)
    if(NOT WAYDISPLAY_BUILD_TESTS)
        message(FATAL_ERROR
            "WAYDISPLAY_REQUIRE_CODEC_TESTS requires WAYDISPLAY_BUILD_TESTS=ON")
    endif()

    set(_waydisplay_missing_codec_backends)
    if(WAYDISPLAY_ENABLE_H265_SERVER_ENCODER AND
       NOT WAYDISPLAY_HAVE_H265_SERVER_ENCODER)
        list(APPEND _waydisplay_missing_codec_backends "H.265 server encoder")
    endif()
    if(WAYDISPLAY_ENABLE_H264_SERVER_ENCODER AND
       NOT WAYDISPLAY_HAVE_H264_SERVER_ENCODER)
        list(APPEND _waydisplay_missing_codec_backends "H.264 server encoder")
    endif()
    if(WAYDISPLAY_ENABLE_H265_CLIENT_DECODER AND
       NOT WAYDISPLAY_HAVE_H265_CLIENT_DECODER)
        list(APPEND _waydisplay_missing_codec_backends "H.265 client decoder")
    endif()
    if(WAYDISPLAY_ENABLE_H264_CLIENT_DECODER AND
       NOT WAYDISPLAY_HAVE_H264_CLIENT_DECODER)
        list(APPEND _waydisplay_missing_codec_backends "H.264 client decoder")
    endif()
    if(WAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER AND
       NOT WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER)
        list(APPEND _waydisplay_missing_codec_backends "VAAPI client decoder")
    endif()

    if(_waydisplay_missing_codec_backends)
        list(JOIN _waydisplay_missing_codec_backends ", "
            _waydisplay_missing_codec_backends_text)
        message(FATAL_ERROR
            "Requested codec test backends are unavailable: "
            "${_waydisplay_missing_codec_backends_text}")
    endif()
endif()

if(WAYDISPLAY_ENABLE_AUDIO)
    pkg_check_modules(OPUS_AUDIO QUIET IMPORTED_TARGET opus)
    if(OPUS_AUDIO_FOUND)
        set(WAYDISPLAY_HAVE_OPUS_AUDIO TRUE)
    else()
        message(STATUS "Opus audio codec disabled: opus not found")
    endif()
    pkg_check_modules(PIPEWIRE_AUDIO QUIET IMPORTED_TARGET libpipewire-0.3>=0.3.50)
    if(PIPEWIRE_AUDIO_FOUND)
        set(WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE TRUE)
    else()
        message(STATUS "PipeWire audio capture disabled: libpipewire-0.3 >= 0.3.50 not found")
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
    src/common/wd_selection.c
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


add_library(waydisplay_server_lifecycle STATIC
    src/server/wd_net_listener.c
    src/server/wd_process.c
    src/server/wd_server_cli.c
    src/server/wd_selection_capture.c
    src/server/wd_selection_delivery.c
)

waydisplay_apply_common_warnings(waydisplay_server_lifecycle)

target_include_directories(waydisplay_server_lifecycle PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(waydisplay_server_lifecycle PUBLIC
    waydisplay_common
)

add_library(waydisplay_server_tile_policy STATIC
    src/server/wd_tile_policy.c
    src/server/wd_dirty_region_scheduler.c
    src/server/wd_async_udp_accounting.c
    src/server/wd_video_transition.c
    src/server/wd_input_correlation.c
    src/server/wd_scene_policy.c
    src/server/wd_connection_identity.c
    src/server/wd_audio_ring.c
    src/server/wd_audio_packetizer.c
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
    src/client/audio_video_sync.cpp
    src/client/content_order.cpp
    src/client/present_telemetry.cpp
    src/client/render_wakeup.cpp
    src/client/selection_sync.cpp
    src/client/stream_ownership.cpp
    src/client/udp_transport_lifecycle.cpp
    src/client/video_transition.cpp
    src/client/video_present_queue.cpp
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
# Video codec libraries
# -----------------------------------------------------------------------------

# Build the public codec wrappers even when FFmpeg support is unavailable so
# client/server targets retain their existing "backend unavailable" behavior.
# Tests that require real codecs are registered only for enabled backends below.
add_library(waydisplay_video_encoder STATIC
    src/server/wd_video_encoder.c
)

waydisplay_apply_common_warnings(waydisplay_video_encoder)

target_include_directories(waydisplay_video_encoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(waydisplay_video_encoder PRIVATE
    WAYDISPLAY_HAVE_H265_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_HAVE_H265_SERVER_ENCODER}>
    WAYDISPLAY_HAVE_H264_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_HAVE_H264_SERVER_ENCODER}>
)

target_link_libraries(waydisplay_video_encoder PUBLIC
    waydisplay_common
)

if(WAYDISPLAY_HAVE_H265_SERVER_ENCODER OR WAYDISPLAY_HAVE_H264_SERVER_ENCODER)
    target_link_libraries(waydisplay_video_encoder PUBLIC
        PkgConfig::FFMPEG_H265_ENCODER
    )
endif()

add_library(waydisplay_video_decoder STATIC
    src/client/video_decoder.cpp
)

waydisplay_apply_common_warnings(waydisplay_video_decoder)

target_include_directories(waydisplay_video_decoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(waydisplay_video_decoder PRIVATE
    WAYDISPLAY_HAVE_H265_CLIENT_DECODER=$<BOOL:${WAYDISPLAY_HAVE_H265_CLIENT_DECODER}>
    WAYDISPLAY_HAVE_H264_CLIENT_DECODER=$<BOOL:${WAYDISPLAY_HAVE_H264_CLIENT_DECODER}>
    WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER=$<BOOL:${WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER}>
)

target_link_libraries(waydisplay_video_decoder PUBLIC
    waydisplay_common
)

if(WAYDISPLAY_HAVE_H265_CLIENT_DECODER OR WAYDISPLAY_HAVE_H264_CLIENT_DECODER)
    target_link_libraries(waydisplay_video_decoder PUBLIC
        PkgConfig::FFMPEG_H265_DECODER
    )
endif()

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
        src/client/audio_playback.cpp
        src/client/sdl_input.cpp
        src/client/sdl_viewer.cpp
    )

    waydisplay_apply_common_warnings(waydisplay_client_sdl)

    target_include_directories(waydisplay_client_sdl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    target_compile_definitions(waydisplay_client_sdl PRIVATE
        WAYDISPLAY_HAVE_OPUS_AUDIO=$<BOOL:${WAYDISPLAY_HAVE_OPUS_AUDIO}>
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

    if(TARGET waydisplay_video_decoder)
        target_link_libraries(waydisplay_client_sdl PRIVATE
            waydisplay_video_decoder
        )
    endif()
    if(WAYDISPLAY_HAVE_OPUS_AUDIO)
        target_link_libraries(waydisplay_client_sdl PRIVATE PkgConfig::OPUS_AUDIO)
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

        # Keep the complete wlroots compile/link contract in one target.  The
        # public wlroots headers include generated Wayland protocol headers, so
        # every production or test target that consumes wd_server.h must both
        # inherit GENERATED_PROTOCOL_DIR and wait for the scanner outputs.
        add_library(waydisplay_wlroots_build_contract INTERFACE)
        add_dependencies(waydisplay_wlroots_build_contract
            waydisplay_wayland_protocols
        )
        target_include_directories(waydisplay_wlroots_build_contract INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
            ${GENERATED_PROTOCOL_DIR}
        )
        target_compile_definitions(waydisplay_wlroots_build_contract INTERFACE
            WLR_USE_UNSTABLE
            WAYDISPLAY_ENABLE_XWAYLAND=$<BOOL:${WAYDISPLAY_ENABLE_XWAYLAND}>
        )
        target_link_libraries(waydisplay_wlroots_build_contract INTERFACE
            waydisplay_common
            PkgConfig::WLROOTS
            PkgConfig::WAYLAND_SERVER
            PkgConfig::XKBCOMMON
            PkgConfig::PIXMAN
            PkgConfig::LIBDRM
            PkgConfig::LIBURING
            Threads::Threads
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
            src/server/wd_audio_capture.c
            src/server/wd_audio_routing.c
            src/server/wd_audio_encoder.c
            src/server/wd_audio_stream.c
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
            src/server/wd_scene_graph.c
            src/server/wd_readback.c
            src/server/wd_pointer.c
        )

        waydisplay_apply_common_warnings(waydisplay_server_wlroots)

        target_compile_definitions(waydisplay_server_wlroots PRIVATE
            WAYDISPLAY_ENABLE_H265_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_ENABLE_H265_SERVER_ENCODER}>
            WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE=$<BOOL:${WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE}>
            WAYDISPLAY_HAVE_OPUS_AUDIO=$<BOOL:${WAYDISPLAY_HAVE_OPUS_AUDIO}>
        )

        target_link_libraries(waydisplay_server_wlroots PRIVATE
            waydisplay_wlroots_build_contract
            waydisplay_server_tile_policy
            waydisplay_server_lifecycle
        )

        if(TARGET waydisplay_video_encoder)
            target_link_libraries(waydisplay_server_wlroots PRIVATE
                waydisplay_video_encoder
            )
        endif()
        if(WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE)
            target_link_libraries(waydisplay_server_wlroots PRIVATE PkgConfig::PIPEWIRE_AUDIO)
        endif()
        if(WAYDISPLAY_HAVE_OPUS_AUDIO)
            target_link_libraries(waydisplay_server_wlroots PRIVATE PkgConfig::OPUS_AUDIO)
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

if(WAYDISPLAY_REQUIRE_WLROOTS_TESTS)
    if(NOT WAYDISPLAY_BUILD_TESTS)
        message(FATAL_ERROR
            "WAYDISPLAY_REQUIRE_WLROOTS_TESTS requires WAYDISPLAY_BUILD_TESTS=ON")
    endif()
    if(NOT TARGET waydisplay_server_wlroots)
        message(FATAL_ERROR
            "WAYDISPLAY_REQUIRE_WLROOTS_TESTS is enabled, but the wlroots "
            "server and scene tests cannot be built")
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
    enable_testing()

    function(waydisplay_add_test)
        cmake_parse_arguments(WAYDISPLAY_TEST
            ""
            "NAME;TARGET;SKIP_RETURN_CODE;TIMEOUT;RESOURCE_LOCK"
            "SOURCES;LIBRARIES;INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;LABELS"
            ${ARGN}
        )

        if(WAYDISPLAY_TEST_UNPARSED_ARGUMENTS)
            message(FATAL_ERROR
                "waydisplay_add_test received unknown arguments: "
                "${WAYDISPLAY_TEST_UNPARSED_ARGUMENTS}")
        endif()
        if(NOT WAYDISPLAY_TEST_NAME)
            message(FATAL_ERROR "waydisplay_add_test requires NAME")
        endif()
        if(NOT WAYDISPLAY_TEST_TARGET)
            message(FATAL_ERROR "waydisplay_add_test requires TARGET")
        endif()
        if(NOT WAYDISPLAY_TEST_SOURCES)
            message(FATAL_ERROR
                "waydisplay_add_test(${WAYDISPLAY_TEST_NAME}) requires SOURCES")
        endif()
        if(TARGET ${WAYDISPLAY_TEST_TARGET})
            message(FATAL_ERROR
                "waydisplay_add_test target already exists: ${WAYDISPLAY_TEST_TARGET}")
        endif()

        add_executable(${WAYDISPLAY_TEST_TARGET} ${WAYDISPLAY_TEST_SOURCES})
        waydisplay_apply_common_warnings(${WAYDISPLAY_TEST_TARGET})

        if(WAYDISPLAY_TEST_INCLUDE_DIRECTORIES)
            target_include_directories(${WAYDISPLAY_TEST_TARGET} PRIVATE
                ${WAYDISPLAY_TEST_INCLUDE_DIRECTORIES})
        endif()
        if(WAYDISPLAY_TEST_LIBRARIES)
            target_link_libraries(${WAYDISPLAY_TEST_TARGET} PRIVATE
                ${WAYDISPLAY_TEST_LIBRARIES})
        endif()
        if(WAYDISPLAY_TEST_COMPILE_DEFINITIONS)
            target_compile_definitions(${WAYDISPLAY_TEST_TARGET} PRIVATE
                ${WAYDISPLAY_TEST_COMPILE_DEFINITIONS})
        endif()

        add_test(NAME ${WAYDISPLAY_TEST_NAME} COMMAND ${WAYDISPLAY_TEST_TARGET})
        if(DEFINED WAYDISPLAY_TEST_TIMEOUT AND
           NOT WAYDISPLAY_TEST_TIMEOUT STREQUAL "")
            set_tests_properties(${WAYDISPLAY_TEST_NAME} PROPERTIES
                TIMEOUT ${WAYDISPLAY_TEST_TIMEOUT})
        else()
            set_tests_properties(${WAYDISPLAY_TEST_NAME} PROPERTIES
                TIMEOUT 10)
        endif()
        if(DEFINED WAYDISPLAY_TEST_RESOURCE_LOCK AND
           NOT WAYDISPLAY_TEST_RESOURCE_LOCK STREQUAL "")
            set_tests_properties(${WAYDISPLAY_TEST_NAME} PROPERTIES
                RESOURCE_LOCK ${WAYDISPLAY_TEST_RESOURCE_LOCK})
        endif()
        if(DEFINED WAYDISPLAY_TEST_SKIP_RETURN_CODE AND
           NOT WAYDISPLAY_TEST_SKIP_RETURN_CODE STREQUAL "")
            set_tests_properties(${WAYDISPLAY_TEST_NAME} PROPERTIES
                SKIP_RETURN_CODE ${WAYDISPLAY_TEST_SKIP_RETURN_CODE})
        endif()
        if(WAYDISPLAY_TEST_LABELS)
            set_tests_properties(${WAYDISPLAY_TEST_NAME} PROPERTIES
                LABELS "${WAYDISPLAY_TEST_LABELS}")
        endif()
        set_property(GLOBAL APPEND PROPERTY WAYDISPLAY_TEST_TARGETS
            ${WAYDISPLAY_TEST_TARGET})
    endfunction()

    waydisplay_add_test(
        NAME waydisplay.client_config
        TARGET waydisplay_test_client_config
        SOURCES tests/test_client_config.cpp
        LIBRARIES waydisplay_client_config
    )

    waydisplay_add_test(
        NAME waydisplay.client_config_matrix
        TARGET waydisplay_test_client_config_matrix
        SOURCES tests/test_client_config_matrix.cpp
        LIBRARIES waydisplay_client_config
    )

    waydisplay_add_test(
        NAME waydisplay.tile_protocol
        TARGET waydisplay_test_tile_protocol
        SOURCES tests/test_tile_protocol.cpp
        LIBRARIES waydisplay_common
    )

    waydisplay_add_test(
        NAME waydisplay.selection_protocol
        TARGET waydisplay_test_selection_protocol
        SOURCES tests/test_selection_protocol.cpp
        LIBRARIES waydisplay_common
        LABELS "clipboard;protocol"
    )

    waydisplay_add_test(
        NAME waydisplay.selection_sync
        TARGET waydisplay_test_selection_sync
        SOURCES tests/test_selection_sync.cpp
        LIBRARIES waydisplay_client_render_planning
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
        LABELS "clipboard;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.selection_capture
        TARGET waydisplay_test_selection_capture
        SOURCES tests/test_selection_capture.cpp
        LIBRARIES waydisplay_server_lifecycle
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "clipboard;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.selection_delivery
        TARGET waydisplay_test_selection_delivery
        SOURCES tests/test_selection_delivery.cpp
        LIBRARIES waydisplay_server_lifecycle
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "clipboard;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.clipboard_sync_lifecycle
        TARGET waydisplay_test_clipboard_sync_lifecycle
        SOURCES tests/test_clipboard_sync_lifecycle.cpp
        LIBRARIES waydisplay_common waydisplay_client_render_planning waydisplay_server_lifecycle
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "clipboard;lifecycle;protocol"
    )

    waydisplay_add_test(
        NAME waydisplay.tile_codec
        TARGET waydisplay_test_tile_codec
        SOURCES tests/test_tile_codec.cpp
        LIBRARIES waydisplay_common
    )

    waydisplay_add_test(
        NAME waydisplay.tile_reassembly
        TARGET waydisplay_test_tile_reassembly
        SOURCES tests/test_tile_reassembly.cpp
        LIBRARIES waydisplay_client_tile_reassembly
    )

    waydisplay_add_test(
        NAME waydisplay.tile_reassembly_edges
        TARGET waydisplay_test_tile_reassembly_edges
        SOURCES tests/test_tile_reassembly_edges.cpp
        LIBRARIES waydisplay_client_tile_reassembly
    )

    waydisplay_add_test(
        NAME waydisplay.server_tile_policy
        TARGET waydisplay_test_server_tile_policy
        SOURCES tests/test_server_tile_policy.cpp
        LIBRARIES waydisplay_server_tile_policy
    )

    waydisplay_add_test(
        NAME waydisplay.net_run_state
        TARGET waydisplay_test_net_run_state
        SOURCES tests/test_net_run_state.c
        LIBRARIES Threads::Threads
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    )

    waydisplay_add_test(
        NAME waydisplay.net_listener
        TARGET waydisplay_test_net_listener
        SOURCES tests/test_net_listener.c
        LIBRARIES waydisplay_server_lifecycle
    )

    waydisplay_add_test(
        NAME waydisplay.process_spawn
        TARGET waydisplay_test_process_spawn
        SOURCES tests/test_process_spawn.c
        LIBRARIES waydisplay_server_lifecycle
    )

    waydisplay_add_test(
        NAME waydisplay.server_cli
        TARGET waydisplay_test_server_cli
        SOURCES tests/test_server_cli.c
        LIBRARIES waydisplay_server_lifecycle
    )

    waydisplay_add_test(
        NAME waydisplay.dirty_region_scheduler
        TARGET waydisplay_test_dirty_region_scheduler
        SOURCES tests/test_dirty_region_scheduler.cpp
        LIBRARIES waydisplay_server_tile_policy
    )

    waydisplay_add_test(
        NAME waydisplay.recovery_policy_edges
        TARGET waydisplay_test_recovery_policy_edges
        SOURCES tests/test_recovery_policy_edges.cpp
        LIBRARIES waydisplay_server_tile_policy
    )

    waydisplay_add_test(
        NAME waydisplay.async_udp_accounting
        TARGET waydisplay_test_async_udp_accounting
        SOURCES tests/test_async_udp_accounting.cpp
        LIBRARIES waydisplay_server_tile_policy
    )

    if(NOT TARGET PkgConfig::LIBURING)
        pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
    endif()
    if(TARGET PkgConfig::LIBURING)
        waydisplay_add_test(
            NAME waydisplay.async_sender_shutdown
            TARGET waydisplay_test_async_sender_shutdown
            SOURCES
                tests/test_async_sender_shutdown.c
                src/server/wd_async_tcp.c
                src/server/wd_async_udp.c
                src/server/wd_async_udp_accounting.c
            LIBRARIES
                waydisplay_common
                PkgConfig::LIBURING
            INCLUDE_DIRECTORIES
                ${CMAKE_CURRENT_SOURCE_DIR}/src/server
                ${CMAKE_CURRENT_SOURCE_DIR}/include
            COMPILE_DEFINITIONS
                WD_ASYNC_TCP_DRAIN_LIMIT=0
                WD_ASYNC_UDP_DRAIN_LIMIT=0
            SKIP_RETURN_CODE 77
            LABELS "network;lifecycle;io_uring"
        )


        waydisplay_add_test(
            NAME waydisplay.client_async_sender_shutdown
            TARGET waydisplay_test_client_async_sender_shutdown
            SOURCES
                tests/test_client_async_sender_shutdown.cpp
                src/client/client_async_tcp.cpp
            LIBRARIES
                waydisplay_common
                PkgConfig::LIBURING
            INCLUDE_DIRECTORIES
                ${CMAKE_CURRENT_SOURCE_DIR}/src/client
                ${CMAKE_CURRENT_SOURCE_DIR}/include
            COMPILE_DEFINITIONS
                WD_CLIENT_ASYNC_TCP_DRAIN_LIMIT=0
            SKIP_RETURN_CODE 77
            LABELS "network;lifecycle;io_uring;client"
        )
    endif()

    waydisplay_add_test(
        NAME waydisplay.audio_ring
        TARGET waydisplay_test_audio_ring
        SOURCES tests/test_audio_ring.cpp
        LIBRARIES waydisplay_server_tile_policy
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    )

    waydisplay_add_test(
        NAME waydisplay.audio_transport
        TARGET waydisplay_test_audio_transport
        SOURCES tests/test_audio_transport.cpp
        LIBRARIES waydisplay_common
    )

    waydisplay_add_test(
        NAME waydisplay.audio_transition_stress
        TARGET waydisplay_test_audio_transition_stress
        SOURCES tests/test_audio_transition_stress.cpp
        TIMEOUT 60
        LIBRARIES
            waydisplay_server_tile_policy
            waydisplay_client_render_planning
            waydisplay_common
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    waydisplay_add_test(
        NAME waydisplay.audio_video_sync
        TARGET waydisplay_test_audio_video_sync
        SOURCES tests/test_audio_video_sync.cpp
        LIBRARIES waydisplay_client_render_planning
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    waydisplay_add_test(
        NAME waydisplay.audio_routing
        TARGET waydisplay_test_audio_routing
        SOURCES
            tests/test_audio_routing.cpp
            src/server/wd_audio_routing.c
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    )

    waydisplay_add_test(
        NAME waydisplay.scene_policy
        TARGET waydisplay_test_scene_policy
        SOURCES tests/test_scene_policy.cpp
        LIBRARIES waydisplay_server_tile_policy
    )

    if(TARGET waydisplay_server_wlroots)
        waydisplay_add_test(
            NAME waydisplay.wlroots_scene_graph
            TARGET waydisplay_test_wlroots_scene_graph
            SOURCES
                tests/test_wlroots_scene_graph.c
                src/server/wd_scene_graph.c
            LIBRARIES waydisplay_wlroots_build_contract
            LABELS "wayland;wlroots;scene"
            TIMEOUT 30
        )

        waydisplay_add_test(
            NAME waydisplay.wlroots_scene_lifecycle
            TARGET waydisplay_test_wlroots_scene_lifecycle
            SOURCES tests/test_wlroots_scene_lifecycle.c
            LIBRARIES waydisplay_wlroots_build_contract
            LABELS "wayland;wlroots;scene;lifecycle"
            TIMEOUT 30
        )
    endif()

    set(_waydisplay_expect_wlroots_tests FALSE)
    if(TARGET waydisplay_server_wlroots)
        set(_waydisplay_expect_wlroots_tests TRUE)
    endif()
    add_test(
        NAME waydisplay.wlroots_scene_test_gating
        COMMAND ${CMAKE_COMMAND}
            -DWAYDISPLAY_TEST_FILE=${CMAKE_BINARY_DIR}/CTestTestfile.cmake
            -DWAYDISPLAY_EXPECT_WLROOTS_TESTS=${_waydisplay_expect_wlroots_tests}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_wlroots_scene_test_gating.cmake
    )
    set_tests_properties(waydisplay.wlroots_scene_test_gating PROPERTIES
        LABELS "wayland;wlroots;cmake"
        TIMEOUT 10
    )

    waydisplay_add_test(
        NAME waydisplay.transition_integrity
        TARGET waydisplay_test_transition_integrity
        SOURCES tests/test_transition_integrity.cpp
        LIBRARIES
            waydisplay_server_tile_policy
            waydisplay_client_render_planning
    )

    waydisplay_add_test(
        NAME waydisplay.render_planning
        TARGET waydisplay_test_render_planning
        SOURCES tests/test_render_planning.cpp
        LIBRARIES
            waydisplay_client_render_planning
            Threads::Threads
    )

    waydisplay_add_test(
        NAME waydisplay.loopback_runtime
        TARGET waydisplay_test_loopback_runtime
        SOURCES tests/test_loopback_runtime.cpp
        TIMEOUT 60
        LIBRARIES waydisplay_client_tile_reassembly
    )

    waydisplay_add_test(
        NAME waydisplay.protocol_fuzz
        TARGET waydisplay_test_protocol_fuzz
        SOURCES tests/test_protocol_fuzz.cpp
        TIMEOUT 60
        LIBRARIES waydisplay_client_tile_reassembly
    )

    waydisplay_add_test(
        NAME waydisplay.reconnect_render_lifecycle
        TARGET waydisplay_test_reconnect_render_lifecycle
        SOURCES tests/test_reconnect_render_lifecycle.cpp
        TIMEOUT 60
        LIBRARIES
            waydisplay_client_render_planning
            waydisplay_server_tile_policy
    )

    waydisplay_add_test(
        NAME waydisplay.video_present_queue
        TARGET waydisplay_test_video_present_queue
        SOURCES tests/test_video_present_queue.cpp
        LIBRARIES waydisplay_client_render_planning
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    waydisplay_add_test(
        NAME waydisplay.render_decisions
        TARGET waydisplay_test_render_decisions
        SOURCES tests/test_render_decisions.cpp
        LIBRARIES
            waydisplay_client_render_planning
            Threads::Threads
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    waydisplay_add_test(
        NAME waydisplay.transition_stress
        TARGET waydisplay_test_transition_stress
        SOURCES tests/test_transition_stress.cpp
        TIMEOUT 60
        LIBRARIES
            waydisplay_client_config
            waydisplay_client_render_planning
            waydisplay_server_tile_policy
    )

    if(WAYDISPLAY_HAVE_H265_SERVER_ENCODER OR WAYDISPLAY_HAVE_H264_SERVER_ENCODER)
        waydisplay_add_test(
            NAME waydisplay.video_encoder
            TARGET waydisplay_test_video_encoder
            SOURCES tests/test_video_encoder.cpp
            LIBRARIES waydisplay_video_encoder
            COMPILE_DEFINITIONS
                WAYDISPLAY_TEST_HAVE_H265_ENCODER=$<BOOL:${WAYDISPLAY_HAVE_H265_SERVER_ENCODER}>
                WAYDISPLAY_TEST_HAVE_H264_ENCODER=$<BOOL:${WAYDISPLAY_HAVE_H264_SERVER_ENCODER}>
            SKIP_RETURN_CODE 77
            LABELS "video;codec;encoder"
            TIMEOUT 30
        )
    endif()

    if(WAYDISPLAY_HAVE_H265_CLIENT_DECODER OR WAYDISPLAY_HAVE_H264_CLIENT_DECODER)
        waydisplay_add_test(
            NAME waydisplay.video_decoder
            TARGET waydisplay_test_video_decoder
            SOURCES tests/test_video_decoder.cpp
            LIBRARIES waydisplay_video_decoder
            COMPILE_DEFINITIONS
                WAYDISPLAY_TEST_HAVE_H265_DECODER=$<BOOL:${WAYDISPLAY_HAVE_H265_CLIENT_DECODER}>
                WAYDISPLAY_TEST_HAVE_H264_DECODER=$<BOOL:${WAYDISPLAY_HAVE_H264_CLIENT_DECODER}>
                WAYDISPLAY_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            SKIP_RETURN_CODE 77
            LABELS "video;codec;decoder"
            TIMEOUT 30
        )
    endif()

    if(WAYDISPLAY_HAVE_H265_SERVER_ENCODER OR WAYDISPLAY_HAVE_H264_SERVER_ENCODER)
        waydisplay_add_test(
            NAME waydisplay.video_encoder_vaapi
            TARGET waydisplay_test_video_encoder_vaapi
            SOURCES tests/test_video_encoder_vaapi.cpp
            LIBRARIES waydisplay_video_encoder
            SKIP_RETURN_CODE 77
            LABELS "video;codec;encoder;hardware;vaapi"
            TIMEOUT 30
            RESOURCE_LOCK waydisplay_gpu
        )
    endif()

    if(WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER AND
       (WAYDISPLAY_HAVE_H265_CLIENT_DECODER OR WAYDISPLAY_HAVE_H264_CLIENT_DECODER))
        waydisplay_add_test(
            NAME waydisplay.video_decoder_vaapi
            TARGET waydisplay_test_video_decoder_vaapi
            SOURCES tests/test_video_decoder_vaapi.cpp
            LIBRARIES waydisplay_video_decoder
            COMPILE_DEFINITIONS
                WAYDISPLAY_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            SKIP_RETURN_CODE 77
            LABELS "video;codec;decoder;hardware;vaapi"
            TIMEOUT 30
            RESOURCE_LOCK waydisplay_gpu
        )
    endif()

    if((WAYDISPLAY_HAVE_H265_SERVER_ENCODER OR WAYDISPLAY_HAVE_H264_SERVER_ENCODER) AND
       (WAYDISPLAY_HAVE_H265_CLIENT_DECODER OR WAYDISPLAY_HAVE_H264_CLIENT_DECODER))
        waydisplay_add_test(
            NAME waydisplay.video_codec_roundtrip
            TARGET waydisplay_test_video_codec_roundtrip
            SOURCES tests/test_video_codec_roundtrip.cpp
            LIBRARIES
                waydisplay_video_encoder
                waydisplay_video_decoder
            SKIP_RETURN_CODE 77
            LABELS "video;codec;roundtrip"
            TIMEOUT 30
        )
    endif()

    foreach(_waydisplay_codec_test IN ITEMS
            video_encoder
            video_decoder
            video_encoder_vaapi
            video_decoder_vaapi
            video_codec_roundtrip)
        if(TARGET waydisplay_test_${_waydisplay_codec_test})
            set(_waydisplay_expect_${_waydisplay_codec_test} TRUE)
        else()
            set(_waydisplay_expect_${_waydisplay_codec_test} FALSE)
        endif()
    endforeach()
    add_test(
        NAME waydisplay.codec_test_gating
        COMMAND ${CMAKE_COMMAND}
            -DWAYDISPLAY_TEST_FILE=${CMAKE_BINARY_DIR}/CTestTestfile.cmake
            -DWAYDISPLAY_EXPECT_VIDEO_ENCODER=${_waydisplay_expect_video_encoder}
            -DWAYDISPLAY_EXPECT_VIDEO_DECODER=${_waydisplay_expect_video_decoder}
            -DWAYDISPLAY_EXPECT_VIDEO_ENCODER_VAAPI=${_waydisplay_expect_video_encoder_vaapi}
            -DWAYDISPLAY_EXPECT_VIDEO_DECODER_VAAPI=${_waydisplay_expect_video_decoder_vaapi}
            -DWAYDISPLAY_EXPECT_VIDEO_CODEC_ROUNDTRIP=${_waydisplay_expect_video_codec_roundtrip}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_codec_test_gating.cmake
    )
    set_tests_properties(waydisplay.codec_test_gating PROPERTIES
        LABELS "video;codec;cmake"
        TIMEOUT 10
    )

    get_property(WAYDISPLAY_TEST_EXECUTABLES GLOBAL PROPERTY
        WAYDISPLAY_TEST_TARGETS)
    list(REMOVE_DUPLICATES WAYDISPLAY_TEST_EXECUTABLES)

    if(NOT WAYDISPLAY_TEST_EXECUTABLES)
        message(FATAL_ERROR
            "WAYDISPLAY_BUILD_TESTS is enabled but no tests were registered")
    endif()

    set(_waydisplay_run_tests_all)
    if(WAYDISPLAY_RUN_TESTS_ON_BUILD)
        set(_waydisplay_run_tests_all ALL)
    endif()

    set(_waydisplay_ctest_arguments
        --output-on-failure
        --no-tests=error
    )
    if(CMAKE_CONFIGURATION_TYPES)
        list(APPEND _waydisplay_ctest_arguments
            --build-config $<CONFIG>
        )
    endif()

    add_custom_target(run_tests ${_waydisplay_run_tests_all}
        COMMAND ${CMAKE_CTEST_COMMAND} ${_waydisplay_ctest_arguments}
        DEPENDS ${WAYDISPLAY_TEST_EXECUTABLES}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running WayDisplay CTest suite"
        USES_TERMINAL
    )
endif()

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
    target_compile_options(waydisplay_fuzz_tile_reassembly PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(waydisplay_fuzz_tile_reassembly PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------

message(STATUS "WayDisplay build options:")
message(STATUS "  WAYDISPLAY_BUILD_CLIENT_SDL=${WAYDISPLAY_BUILD_CLIENT_SDL}")
message(STATUS "  WAYDISPLAY_BUILD_WLROOTS_SERVER=${WAYDISPLAY_BUILD_WLROOTS_SERVER}")
message(STATUS "  WAYDISPLAY_BUILD_LEGACY_PROTO=${WAYDISPLAY_BUILD_LEGACY_PROTO}")
message(STATUS "  WAYDISPLAY_BUILD_TESTS=${WAYDISPLAY_BUILD_TESTS}")
message(STATUS "  WAYDISPLAY_RUN_TESTS_ON_BUILD=${WAYDISPLAY_RUN_TESTS_ON_BUILD}")
message(STATUS "  WAYDISPLAY_BUILD_FUZZERS=${WAYDISPLAY_BUILD_FUZZERS}")
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
