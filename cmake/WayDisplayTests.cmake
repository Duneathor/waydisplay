# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if(WAYDISPLAY_BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_test(
        NAME waydisplay.cmake_build_profiles
        COMMAND ${CMAKE_COMMAND}
            -DWAYDISPLAY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DWAYDISPLAY_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-profile-contract
            -DWAYDISPLAY_C_COMPILER=${CMAKE_C_COMPILER}
            -DWAYDISPLAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_build_profiles.cmake
    )
    set_tests_properties(waydisplay.cmake_build_profiles PROPERTIES
        TIMEOUT 120
        LABELS "unit;cmake;build"
    )

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
        set(_waydisplay_test_labels ${WAYDISPLAY_TEST_LABELS})
        set(_waydisplay_has_tier FALSE)
        foreach(_waydisplay_tier IN ITEMS unit integration stress fuzz hardware)
            if(_waydisplay_tier IN_LIST _waydisplay_test_labels)
                set(_waydisplay_has_tier TRUE)
            endif()
        endforeach()
        if(NOT _waydisplay_has_tier)
            list(PREPEND _waydisplay_test_labels unit)
        endif()
        set_tests_properties(${WAYDISPLAY_TEST_NAME} PROPERTIES
            LABELS "${_waydisplay_test_labels}")
        set_property(GLOBAL APPEND PROPERTY WAYDISPLAY_TEST_TARGETS
            ${WAYDISPLAY_TEST_TARGET})
    endfunction()

    waydisplay_add_test(
        NAME waydisplay.cpp20_branch_hints
        TARGET waydisplay_test_cpp20_branch_hints
        SOURCES tests/test_cpp20_branch_hints.cpp
        LABELS "unit;build;cpp20"
    )

    waydisplay_add_test(
        NAME waydisplay.client_config
        TARGET waydisplay_test_client_config
        SOURCES tests/test_client_config.cpp
        LIBRARIES waydisplay_client_config
    )

    waydisplay_add_test(
        NAME waydisplay.client_cli
        TARGET waydisplay_test_client_cli
        SOURCES tests/test_client_cli.cpp
        LIBRARIES waydisplay_client_config
        LABELS "cli;client"
    )

    waydisplay_add_test(
        NAME waydisplay.client_config_matrix
        TARGET waydisplay_test_client_config_matrix
        SOURCES tests/test_client_config_matrix.cpp
        LIBRARIES waydisplay_client_config
    )

    waydisplay_add_test(
        NAME waydisplay.protocol_descriptor_matrix
        TARGET waydisplay_test_protocol_descriptor_matrix
        SOURCES tests/test_protocol_descriptor_matrix.cpp
        LIBRARIES waydisplay_common
        LABELS "unit;protocol;network"
    )

    waydisplay_add_test(
        NAME waydisplay.video_adaptive_cadence
        TARGET waydisplay_test_video_adaptive_cadence
        SOURCES tests/test_video_adaptive_cadence.cpp
        LIBRARIES waydisplay_server_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "unit;video;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.video_inplace_recovery
        TARGET waydisplay_test_video_inplace_recovery
        SOURCES tests/test_video_inplace_recovery.cpp
        LIBRARIES waydisplay_server_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "unit;video;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.video_feedback_protocol
        TARGET waydisplay_test_video_feedback_protocol
        SOURCES tests/test_video_feedback_protocol.cpp
        LIBRARIES waydisplay_common
        LABELS "unit;protocol;video;network"
    )

    waydisplay_add_test(
        NAME waydisplay.video_scrub_recovery
        TARGET waydisplay_test_video_scrub_recovery
        SOURCES tests/test_video_scrub_recovery.cpp
        LIBRARIES waydisplay_client_runtime waydisplay_server_runtime
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "integration;client;server;video;lifecycle"
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
        LIBRARIES waydisplay_client_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
        LABELS "clipboard;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.selection_capture
        TARGET waydisplay_test_selection_capture
        SOURCES tests/test_selection_capture.cpp
        LIBRARIES waydisplay_server_support
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "clipboard;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.selection_delivery
        TARGET waydisplay_test_selection_delivery
        SOURCES tests/test_selection_delivery.cpp
        LIBRARIES waydisplay_server_support
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "clipboard;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.clipboard_sync_lifecycle
        TARGET waydisplay_test_clipboard_sync_lifecycle
        SOURCES tests/test_clipboard_sync_lifecycle.cpp
        LIBRARIES waydisplay_common waydisplay_client_runtime waydisplay_server_support
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
        NAME waydisplay.readback_regions
        TARGET waydisplay_test_readback_regions
        SOURCES tests/test_readback_regions.c
        LIBRARIES waydisplay_server_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "scene;video"
    )

    waydisplay_add_test(
        NAME waydisplay.bandwidth_plan
        TARGET waydisplay_test_bandwidth_plan
        SOURCES tests/test_bandwidth_plan.cpp
        LIBRARIES waydisplay_server_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "unit;network;server;video"
    )

    waydisplay_add_test(
        NAME waydisplay.server_tile_policy
        TARGET waydisplay_test_server_tile_policy
        SOURCES tests/test_server_tile_policy.cpp
        LIBRARIES waydisplay_server_runtime
    )

    waydisplay_add_test(
        NAME waydisplay.client_session
        TARGET waydisplay_test_client_session
        SOURCES tests/test_client_session.c
        LIBRARIES waydisplay_client_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
        LABELS "client;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.net_run_state
        TARGET waydisplay_test_net_run_state
        SOURCES tests/test_net_run_state.c
        LIBRARIES Threads::Threads
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
    )

    waydisplay_add_test(
        NAME waydisplay.eventfd_wakeup
        TARGET waydisplay_test_eventfd_wakeup
        SOURCES tests/test_eventfd_wakeup.c
        LIBRARIES waydisplay_common Threads::Threads
        LABELS "lifecycle;network;threading"
    )

    add_test(
        NAME waydisplay.threaded_runtime_contracts
        COMMAND ${CMAKE_COMMAND}
            -DWAYDISPLAY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_threaded_runtime_contracts.cmake
    )
    set_tests_properties(waydisplay.threaded_runtime_contracts PROPERTIES
        TIMEOUT 10
        LABELS "unit;cmake;lifecycle;threading"
    )

    waydisplay_add_test(
        NAME waydisplay.control_handshake_runtime
        TARGET waydisplay_test_control_handshake_runtime
        SOURCES tests/test_control_handshake_runtime.cpp
        LIBRARIES waydisplay_common waydisplay_server_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "integration;network;protocol;server"
    )

    waydisplay_add_test(
        NAME waydisplay.net_listener
        TARGET waydisplay_test_net_listener
        SOURCES tests/test_net_listener.c
        LIBRARIES waydisplay_server_support
    )

    waydisplay_add_test(
        NAME waydisplay.process_spawn
        TARGET waydisplay_test_process_spawn
        SOURCES tests/test_process_spawn.c
        LIBRARIES waydisplay_server_support
    )

    waydisplay_add_test(
        NAME waydisplay.server_cli
        TARGET waydisplay_test_server_cli
        SOURCES tests/test_server_cli.c
        LIBRARIES waydisplay_server_support
    )

    waydisplay_add_test(
        NAME waydisplay.dirty_region_scheduler
        TARGET waydisplay_test_dirty_region_scheduler
        SOURCES tests/test_dirty_region_scheduler.cpp
        LIBRARIES waydisplay_server_runtime
    )

    waydisplay_add_test(
        NAME waydisplay.frame_pacing
        TARGET waydisplay_test_frame_pacing
        SOURCES tests/test_frame_pacing.c
        LIBRARIES waydisplay_server_runtime
    )

    waydisplay_add_test(
        NAME waydisplay.recovery_policy_edges
        TARGET waydisplay_test_recovery_policy_edges
        SOURCES tests/test_recovery_policy_edges.cpp
        LIBRARIES waydisplay_server_runtime
    )

    add_test(
        NAME waydisplay.io_uring_5_14_contract
        COMMAND ${CMAKE_COMMAND}
            -DWAYDISPLAY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_io_uring_5_14_contract.cmake
    )
    set_tests_properties(waydisplay.io_uring_5_14_contract PROPERTIES
        LABELS "unit;cmake;io_uring"
        TIMEOUT 10
    )

    waydisplay_add_test(
        NAME waydisplay.session_boundary_concurrency
        TARGET waydisplay_test_session_boundary_concurrency
        SOURCES tests/test_session_boundary_concurrency.cpp
        LIBRARIES waydisplay_client_runtime waydisplay_server_runtime Threads::Threads
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "integration;lifecycle;network;threading;client;server"
        TIMEOUT 30
    )

    waydisplay_add_test(
        NAME waydisplay.async_udp_accounting
        TARGET waydisplay_test_async_udp_accounting
        SOURCES tests/test_async_udp_accounting.cpp
        LIBRARIES waydisplay_server_runtime
    )

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

    waydisplay_add_test(
        NAME waydisplay.audio_ring
        TARGET waydisplay_test_audio_ring
        SOURCES tests/test_audio_ring.cpp
        LIBRARIES waydisplay_server_runtime
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
            waydisplay_server_runtime
            waydisplay_client_runtime
            waydisplay_common
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    waydisplay_add_test(
        NAME waydisplay.audio_startup_integration
        TARGET waydisplay_test_audio_startup_integration
        SOURCES tests/test_audio_startup_integration.cpp
        LIBRARIES waydisplay_client_runtime waydisplay_server_runtime
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "integration;audio;video;client;server;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.audio_video_sync
        TARGET waydisplay_test_audio_video_sync
        SOURCES tests/test_audio_video_sync.cpp
        LIBRARIES waydisplay_client_runtime
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
        LIBRARIES waydisplay_server_runtime
    )

    if(TARGET waydisplay_server_wlroots)
        waydisplay_add_test(
            NAME waydisplay.wlroots_scene_graph
            TARGET waydisplay_test_wlroots_scene_graph
            SOURCES
                tests/test_wlroots_scene_graph.c
                src/server/wd_scene_graph.c
            LIBRARIES waydisplay_wlroots_dependencies
            LABELS "wayland;wlroots;scene"
            TIMEOUT 30
        )

        waydisplay_add_test(
            NAME waydisplay.wlroots_scene_lifecycle
            TARGET waydisplay_test_wlroots_scene_lifecycle
            SOURCES tests/test_wlroots_scene_lifecycle.c
            LIBRARIES waydisplay_wlroots_dependencies
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
        LABELS "unit;wayland;wlroots;cmake"
        TIMEOUT 10
    )

    waydisplay_add_test(
        NAME waydisplay.transition_integrity
        TARGET waydisplay_test_transition_integrity
        SOURCES tests/test_transition_integrity.cpp
        LIBRARIES
            waydisplay_server_runtime
            waydisplay_client_runtime
    )

    waydisplay_add_test(
        NAME waydisplay.client_runtime_linkage
        TARGET waydisplay_test_client_runtime_linkage
        SOURCES tests/test_client_runtime_linkage.cpp
        LIBRARIES waydisplay_client_runtime
        LABELS "client;linkage;lifecycle"
    )

    waydisplay_add_test(
        NAME waydisplay.render_planning
        TARGET waydisplay_test_render_planning
        SOURCES tests/test_render_planning.cpp
        LIBRARIES waydisplay_client_runtime
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
        NAME waydisplay.stream_lifecycle_scenarios
        TARGET waydisplay_test_stream_lifecycle_scenarios
        SOURCES tests/test_stream_lifecycle_scenarios.cpp
        LIBRARIES waydisplay_client_runtime waydisplay_server_runtime
        INCLUDE_DIRECTORIES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/client
            ${CMAKE_CURRENT_SOURCE_DIR}/src/server
        LABELS "integration;lifecycle;client;server;video"
    )

    waydisplay_add_test(
        NAME waydisplay.reconnect_render_lifecycle
        TARGET waydisplay_test_reconnect_render_lifecycle
        SOURCES tests/test_reconnect_render_lifecycle.cpp
        TIMEOUT 60
        LIBRARIES
            waydisplay_client_runtime
            waydisplay_server_runtime
    )

    waydisplay_add_test(
        NAME waydisplay.video_packet_validation
        TARGET waydisplay_test_video_packet_validation
        SOURCES tests/test_video_packet_validation.cpp
        LIBRARIES waydisplay_client_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
        LABELS "unit;client;video;protocol"
    )

    waydisplay_add_test(
        NAME waydisplay.video_present_queue
        TARGET waydisplay_test_video_present_queue
        SOURCES tests/test_video_present_queue.cpp
        LIBRARIES waydisplay_client_runtime
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/client
    )

    waydisplay_add_test(
        NAME waydisplay.render_decisions
        TARGET waydisplay_test_render_decisions
        SOURCES tests/test_render_decisions.cpp
        LIBRARIES
            waydisplay_client_runtime
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
            waydisplay_client_runtime
            waydisplay_server_runtime
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
            NAME waydisplay.video_resize_roundtrip
            TARGET waydisplay_test_video_resize_roundtrip
            SOURCES tests/test_video_resize_roundtrip.cpp
            LIBRARIES
                waydisplay_video_encoder
                waydisplay_video_decoder
                waydisplay_client_runtime
            SKIP_RETURN_CODE 77
            LABELS "integration;video;codec;roundtrip;resize"
            TIMEOUT 60
        )

        get_target_property(_waydisplay_video_resize_links
            waydisplay_test_video_resize_roundtrip LINK_LIBRARIES)
        if(NOT "waydisplay_client_runtime" IN_LIST _waydisplay_video_resize_links)
            message(FATAL_ERROR
                "waydisplay_test_video_resize_roundtrip must link "
                "waydisplay_client_runtime for video packet validation")
        endif()

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
            video_resize_roundtrip
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
            -DWAYDISPLAY_EXPECT_VIDEO_RESIZE_ROUNDTRIP=${_waydisplay_expect_video_resize_roundtrip}
            -DWAYDISPLAY_EXPECT_VIDEO_CODEC_ROUNDTRIP=${_waydisplay_expect_video_codec_roundtrip}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_codec_test_gating.cmake
    )
    set_tests_properties(waydisplay.codec_test_gating PROPERTIES
        LABELS "unit;video;codec;cmake"
        TIMEOUT 10
    )

    add_test(
        NAME waydisplay.runtime_target_gating
        COMMAND ${CMAKE_COMMAND}
            -DWAYDISPLAY_CLIENT_TARGET_EXISTS=$<BOOL:$<TARGET_EXISTS:waydisplay_client_sdl>>
            -DWAYDISPLAY_SERVER_TARGET_EXISTS=$<BOOL:$<TARGET_EXISTS:waydisplay_server_wlroots>>
            -DWAYDISPLAY_REQUIRE_RUNTIME_TARGETS=${WAYDISPLAY_REQUIRE_RUNTIME_TARGETS}
            -DWAYDISPLAY_BUILD_CLIENT_SDL=${WAYDISPLAY_BUILD_CLIENT_SDL}
            -DWAYDISPLAY_BUILD_WLROOTS_SERVER=${WAYDISPLAY_BUILD_WLROOTS_SERVER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_runtime_target_gating.cmake
    )
    set_tests_properties(waydisplay.runtime_target_gating PROPERTIES
        LABELS "unit;cmake;client;server"
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
