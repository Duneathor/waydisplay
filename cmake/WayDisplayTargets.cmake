# -----------------------------------------------------------------------------
# Common library
# -----------------------------------------------------------------------------

pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_library(waydisplay_common STATIC
    src/common/wd_time.c
    src/common/wd_log.c
    src/common/wd_io_uring.c
    src/common/wd_net.c
    src/common/wd_protocol_codec.c
    src/common/wd_protocol_dispatch.c
    src/common/wd_selection.c
    src/common/wd_tile.c
    src/common/wd_zstd.c
)

waydisplay_apply_common_warnings(waydisplay_common)

target_include_directories(waydisplay_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(waydisplay_common PUBLIC
    PkgConfig::ZSTD
    PkgConfig::LIBURING
)


add_library(waydisplay_server_support STATIC
    src/server/wd_net_listener.c
    src/server/wd_process.c
    src/server/wd_server_cli.c
    src/server/wd_selection_capture.c
    src/server/wd_selection_delivery.c
)

waydisplay_apply_common_warnings(waydisplay_server_support)

target_include_directories(waydisplay_server_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(waydisplay_server_support PUBLIC
    waydisplay_common
)

add_library(waydisplay_server_runtime STATIC
    src/server/wd_tile_policy.c
    src/server/wd_dirty_region_scheduler.c
    src/server/wd_frame_pacing.c
    src/server/wd_async_udp_accounting.c
    src/server/wd_video_transition.c
    src/server/wd_input_correlation.c
    src/server/wd_scene_policy.c
    src/server/wd_readback_regions.c
    src/server/wd_connection_identity.c
    src/server/wd_audio_ring.c
    src/server/wd_audio_packetizer.c
)

waydisplay_apply_common_warnings(waydisplay_server_runtime)

target_include_directories(waydisplay_server_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(waydisplay_client_config STATIC
    src/client/client_cli.cpp
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

add_library(waydisplay_client_runtime STATIC
    src/client/client_session.c
    src/client/render_planning.cpp
    src/client/audio_video_sync.c
    src/client/content_order.cpp
    src/client/present_telemetry.cpp
    src/client/render_wakeup.cpp
    src/client/selection_sync.cpp
    src/client/stream_ownership.c
    src/client/video_transition.c
    src/client/video_present_queue.cpp
)

waydisplay_apply_common_warnings(waydisplay_client_runtime)

target_include_directories(waydisplay_client_runtime PUBLIC
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
        PkgConfig::FFMPEG_VIDEO_ENCODER
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
        PkgConfig::FFMPEG_VIDEO_DECODER
    )
endif()

# -----------------------------------------------------------------------------
# SDL client
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_CLIENT_SDL)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    add_executable(waydisplay_client_sdl
        src/client/main_sdl.cpp
        src/client/client_net.cpp
        src/client/client_receive.cpp
        src/client/client_transport.cpp
        src/client/client_telemetry.cpp
        src/client/client_async_tcp.cpp
        src/client/client_async_udp.cpp
        src/client/audio_playback.cpp
        src/client/sdl_input.cpp
        src/client/sdl_viewer.cpp
    )
    set_target_properties(waydisplay_client_sdl PROPERTIES
        OUTPUT_NAME waydisplay-client
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
        waydisplay_client_runtime
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

    find_program(WAYLAND_SCANNER wayland-scanner)

    if(WAYDISPLAY_WLROOTS_FOUND
       AND WAYLAND_SERVER_FOUND
       AND WAYLAND_PROTOCOLS_FOUND
       AND XKBCOMMON_FOUND
       AND PIXMAN_FOUND
       AND LIBDRM_FOUND
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
        add_library(waydisplay_wlroots_dependencies INTERFACE)
        add_dependencies(waydisplay_wlroots_dependencies
            waydisplay_wayland_protocols
        )
        target_include_directories(waydisplay_wlroots_dependencies INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
            ${GENERATED_PROTOCOL_DIR}
        )
        target_compile_definitions(waydisplay_wlroots_dependencies INTERFACE
            WLR_USE_UNSTABLE
            WAYDISPLAY_ENABLE_XWAYLAND=$<BOOL:${WAYDISPLAY_ENABLE_XWAYLAND}>
        )
        target_link_libraries(waydisplay_wlroots_dependencies INTERFACE
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
            src/server/wd_stream_controller.c
            src/server/wd_stream_frame_worker.c
            src/server/wd_stream_video.c
            src/server/wd_stream_telemetry.c
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
        set_target_properties(waydisplay_server_wlroots PROPERTIES
            OUTPUT_NAME waydisplay-server
        )

        waydisplay_apply_common_warnings(waydisplay_server_wlroots)

        target_compile_definitions(waydisplay_server_wlroots PRIVATE
            WAYDISPLAY_ENABLE_H265_SERVER_ENCODER=$<BOOL:${WAYDISPLAY_ENABLE_H265_SERVER_ENCODER}>
            WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE=$<BOOL:${WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE}>
            WAYDISPLAY_HAVE_OPUS_AUDIO=$<BOOL:${WAYDISPLAY_HAVE_OPUS_AUDIO}>
        )

        target_link_libraries(waydisplay_server_wlroots PRIVATE
            waydisplay_wlroots_dependencies
            waydisplay_server_runtime
            waydisplay_server_support
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
            "Skipping waydisplay-server because one or more dependencies were not found.\n"
            "Required wlroots pkg-config module: wlroots-0.19\n"
            "Required pkg-config modules: wayland-server, wayland-protocols, xkbcommon, pixman-1, libdrm\n"
            "Required executable: wayland-scanner\n"
            "Found status:\n"
            "  wlroots:           ${WAYDISPLAY_WLROOTS_FOUND}\n"
            "  wlroots module:    ${WAYDISPLAY_WLROOTS_MODULE}\n"
            "  wayland-server:    ${WAYLAND_SERVER_FOUND}\n"
            "  wayland-protocols: ${WAYLAND_PROTOCOLS_FOUND}\n"
            "  xkbcommon:         ${XKBCOMMON_FOUND}\n"
            "  pixman-1:          ${PIXMAN_FOUND}\n"
            "  libdrm:            ${LIBDRM_FOUND}\n"
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
