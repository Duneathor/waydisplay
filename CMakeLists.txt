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
option(WAYDISPLAY_ENABLE_LOGGING "Enable WayDisplay runtime logging" ON)
option(WAYDISPLAY_ENABLE_DEBUG_LOGGING "Enable WayDisplay debug-level logging" OFF)
option(WAYDISPLAY_ENABLE_XWAYLAND "Enable Xwayland support in the wlroots server" ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

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
        wlroots-0.18
        wlroots0.18
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

# -----------------------------------------------------------------------------
# SDL client
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_CLIENT_SDL)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    add_executable(waydisplay_client_sdl
        src/client/main_sdl.cpp
        src/client/client_net.cpp
        src/client/tile_reassembly.cpp
        src/client/sdl_input.cpp
        src/client/sdl_viewer.cpp
    )

    waydisplay_apply_common_warnings(waydisplay_client_sdl)

    target_include_directories(waydisplay_client_sdl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    target_link_libraries(waydisplay_client_sdl PRIVATE
        waydisplay_common
        PkgConfig::SDL2
        Threads::Threads
    )
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

        add_executable(waydisplay_server_wlroots
            ${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.c
            ${GENERATED_PROTOCOL_DIR}/xdg-dialog-v1-protocol.c
            src/server/main_wlroots.c
            src/server/wd_server.c
            src/server/wd_server_net.c
            src/server/wd_stream.c
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
        )

        target_include_directories(waydisplay_server_wlroots PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
            ${GENERATED_PROTOCOL_DIR}
        )

        target_link_libraries(waydisplay_server_wlroots PRIVATE
            waydisplay_common
            PkgConfig::WLROOTS
            PkgConfig::WAYLAND_SERVER
            PkgConfig::XKBCOMMON
            PkgConfig::PIXMAN
            PkgConfig::LIBDRM
            Threads::Threads
        )
    else()
        message(WARNING
            "Skipping waydisplay_server_wlroots because one or more dependencies were not found.\n"
            "Tried wlroots pkg-config modules: wlroots, wlroots-0.19, wlroots0.19, wlroots-0.18, wlroots0.18\n"
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
# Summary
# -----------------------------------------------------------------------------

message(STATUS "WayDisplay build options:")
message(STATUS "  WAYDISPLAY_BUILD_CLIENT_SDL=${WAYDISPLAY_BUILD_CLIENT_SDL}")
message(STATUS "  WAYDISPLAY_BUILD_WLROOTS_SERVER=${WAYDISPLAY_BUILD_WLROOTS_SERVER}")
message(STATUS "  WAYDISPLAY_BUILD_LEGACY_PROTO=${WAYDISPLAY_BUILD_LEGACY_PROTO}")
message(STATUS "  WAYDISPLAY_ENABLE_LOGGING=${WAYDISPLAY_ENABLE_LOGGING}")
message(STATUS "  WAYDISPLAY_ENABLE_DEBUG_LOGGING=${WAYDISPLAY_ENABLE_DEBUG_LOGGING}")
message(STATUS "  WAYDISPLAY_ENABLE_XWAYLAND=${WAYDISPLAY_ENABLE_XWAYLAND}")

if(DEFINED WAYDISPLAY_WLROOTS_MODULE)
    message(STATUS "  WAYDISPLAY_WLROOTS_MODULE=${WAYDISPLAY_WLROOTS_MODULE}")
endif()
