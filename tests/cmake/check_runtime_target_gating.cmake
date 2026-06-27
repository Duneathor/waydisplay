if(NOT DEFINED WAYDISPLAY_CLIENT_TARGET_EXISTS OR
   NOT DEFINED WAYDISPLAY_SERVER_TARGET_EXISTS OR
   NOT DEFINED WAYDISPLAY_REQUIRE_RUNTIME_TARGETS OR
   NOT DEFINED WAYDISPLAY_BUILD_CLIENT_SDL OR
   NOT DEFINED WAYDISPLAY_BUILD_WLROOTS_SERVER)
    message(FATAL_ERROR "runtime target gating inputs are incomplete")
endif()

if(WAYDISPLAY_BUILD_CLIENT_SDL AND WAYDISPLAY_REQUIRE_RUNTIME_TARGETS AND NOT WAYDISPLAY_CLIENT_TARGET_EXISTS)
    message(FATAL_ERROR "full-runtime configuration requested the SDL client but did not create waydisplay_client_sdl")
endif()
if(NOT WAYDISPLAY_BUILD_CLIENT_SDL AND WAYDISPLAY_CLIENT_TARGET_EXISTS)
    message(FATAL_ERROR "waydisplay_client_sdl exists even though the SDL client was disabled")
endif()

if(WAYDISPLAY_BUILD_WLROOTS_SERVER AND WAYDISPLAY_REQUIRE_RUNTIME_TARGETS AND NOT WAYDISPLAY_SERVER_TARGET_EXISTS)
    message(FATAL_ERROR "full-runtime configuration requested the wlroots server but did not create waydisplay_server_wlroots")
endif()
if(NOT WAYDISPLAY_BUILD_WLROOTS_SERVER AND WAYDISPLAY_SERVER_TARGET_EXISTS)
    message(FATAL_ERROR "waydisplay_server_wlroots exists even though the wlroots server was disabled")
endif()
