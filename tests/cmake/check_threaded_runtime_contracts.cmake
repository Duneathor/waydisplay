if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

function(read_source relative out_var)
    file(READ "${WAYDISPLAY_SOURCE_DIR}/${relative}" content)
    set(${out_var} "${content}" PARENT_SCOPE)
endfunction()

function(require_absent text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found forbidden text '${pattern}'")
    endif()
endfunction()

read_source("src/server/wd_server.c" server_source)
string(REGEX MATCH "void wd_server_wake_input\\([^}]+\\}" wake_function "${server_source}")
if(wake_function STREQUAL "")
    message(FATAL_ERROR "could not locate wd_server_wake_input")
endif()
require_absent("${wake_function}" "pthread_mutex_lock" "eventfd wake path must remain lock-free")

read_source("src/server/wd_stream_video.c" video_source)
require_absent("${video_source}" "memcpy(worker->pending_job.pixels, server->framebuffer_xrgb8888"
               "video publication must not copy the framebuffer while net.lock is held")

read_source("src/server/wd_stream_telemetry.c" telemetry_source)
require_absent("${telemetry_source}" "wd_stream_policy_update_mode_locked"
               "telemetry must not own stream mode transitions")
require_absent("${telemetry_source}" "wd_stream_advance_content_epoch_locked"
               "telemetry must not advance content ownership")

read_source("src/client/client_net.cpp" client_source)
string(FIND "${client_source}" "void client_network_reader_main" network_start)
string(FIND "${client_source}" "void client_promote_deferred_summary_retransmits" network_end)
if(network_start EQUAL -1 OR network_end EQUAL -1 OR network_end LESS network_start)
    message(FATAL_ERROR "could not locate client_network_reader_main")
endif()
math(EXPR network_length "${network_end} - ${network_start}")
string(SUBSTRING "${client_source}" ${network_start} ${network_length} network_reader)
require_absent("${network_reader}" "client_video_decoder_decode"
               "network RX worker must not decode video")
require_absent("${network_reader}" "client_audio_decode_packet"
               "network RX worker must not decode audio")

read_source("src/client/client_async_udp.cpp" udp_source)
require_absent("${udp_source}" "SocketStillOwned"
               "UDP ring teardown must not leave socket ownership unresolved")

read_source("src/server/wd_server_net.c" server_net_source)
string(FIND "${server_net_source}" "memset(&net->stats, 0, sizeof(net->stats));" session_stats_reset)
string(FIND "${server_net_source}" "net->connection_token     = connection_token;" session_identity_start)
if(session_stats_reset EQUAL -1 OR session_identity_start EQUAL -1 OR session_stats_reset GREATER session_identity_start)
    message(FATAL_ERROR "client interval feedback must be reset before publishing a new connection identity")
endif()
string(FIND "${server_net_source}" "wd_stream_policy_begin_session(&net->stream_policy, &hello, net->content_epoch);" session_policy_begin)
if(session_policy_begin EQUAL -1)
    message(FATAL_ERROR "new connections must enter through the centralized stream-policy session boundary")
endif()

string(FIND "${server_net_source}"
       "wd_server_request_display_mode(server, requested_width, requested_height, requested_refresh_hz)"
       client_mode_request)
if(client_mode_request EQUAL -1)
    message(FATAL_ERROR "accepted client cadence must configure the compositor display mode")
endif()

read_source("src/server/wd_frame_pacing.c" frame_pacing_source)
string(FIND "${frame_pacing_source}" "wd_frame_rate_normalize_client_request" cadence_normalizer)
if(cadence_normalizer EQUAL -1)
    message(FATAL_ERROR "client cadence normalization must remain centralized")
endif()

read_source("src/server/wd_server_internal.h" server_internal_source)
require_absent("${server_internal_source}" "udp_rate_bytes_per_second"
               "aggregate tile-media state must not be mislabeled as a UDP link rate")
require_absent("${server_net_source}" "adaptive_udp_rate_kib_per_sec"
               "connection logs must expose the class plan rather than the legacy UDP-rate label")

read_source("src/server/wd_stream.c" stream_source)
string(FIND "${stream_source}" "void wd_stream_policy_begin_session" begin_session_start)
string(FIND "${stream_source}" "const char* wd_stream_mode_name" begin_session_end)
if(begin_session_start EQUAL -1 OR begin_session_end EQUAL -1 OR begin_session_end LESS begin_session_start)
    message(FATAL_ERROR "could not locate wd_stream_policy_begin_session")
endif()
math(EXPR begin_session_length "${begin_session_end} - ${begin_session_start}")
string(SUBSTRING "${stream_source}" ${begin_session_start} ${begin_session_length} begin_session_function)
foreach(required_field
        "tile_refresh_pending"
        "video_bootstrap_pending"
        "video_bootstrap_content_epoch"
        "tile_recovery_content_epoch"
        "tile_recovery_framebuffer_generation"
        "tile_recovery_live_damage_deferred"
        "planned_recovery_resume_video"
        "video_recovery_class")
    string(FIND "${begin_session_function}" "${required_field}" field_position)
    if(field_position EQUAL -1)
        message(FATAL_ERROR "stream-policy session boundary must initialize ${required_field}")
    endif()
endforeach()

foreach(required_stream_contract
        "wd_video_cadence_downshift_target"
        "wd_video_cadence_upshift_target"
        "WD_STREAM_VIDEO_FPS_DEADBAND")
    string(FIND "${stream_source}" "${required_stream_contract}" stream_contract_position)
    if(stream_contract_position EQUAL -1)
        message(FATAL_ERROR "video cadence must use ${required_stream_contract}")
    endif()
endforeach()
require_absent("${stream_source}"
               "wd_stream_policy_update_frame_rate_locked(policy, stats, true, true, \"immediate client decoder overload\")"
               "decoder overload must use the video-specific cadence controller")
foreach(required_video_contract
        "planned_recovery_resume_video"
        "tile_recovery_framebuffer_generation"
        "tile_recovery_live_damage_deferred")
    string(FIND "${video_source}" "${required_video_contract}" video_contract_position)
    if(video_contract_position EQUAL -1)
        message(FATAL_ERROR "planned resize recovery must retain ${required_video_contract}")
    endif()
endforeach()

read_source("src/client/sdl_viewer.cpp" sdl_viewer_source)
require_absent("${sdl_viewer_source}" "tile_content_epoch_presented.store"
               "tile presentation acknowledgements must never regress")
require_absent("${sdl_viewer_source}" "video_content_epoch_presented.store"
               "video presentation acknowledgements must never regress")
string(FIND "${sdl_viewer_source}" "record_atomic_max(state.stats.tile_content_epoch_presented" tile_epoch_max)
string(FIND "${sdl_viewer_source}" "record_atomic_max(state.stats.video_content_epoch_presented" video_epoch_max)
if(tile_epoch_max EQUAL -1 OR video_epoch_max EQUAL -1)
    message(FATAL_ERROR "presented content epochs must be published monotonically")
endif()

foreach(required_render_contract
        "fallback_texture"
        "client_tile_frame_complete"
        "client_render_surface_handoff_decide")
    string(FIND "${sdl_viewer_source}" "${required_render_contract}" render_contract_position)
    if(render_contract_position EQUAL -1)
        message(FATAL_ERROR "resize rendering must retain ${required_render_contract}")
    endif()
endforeach()
string(FIND "${sdl_viewer_source}" "bool apply_pending_server_config" config_apply_start)
string(FIND "${sdl_viewer_source}" "bool upload_argb_texture_locked" config_apply_end)
if(config_apply_start EQUAL -1 OR config_apply_end EQUAL -1 OR config_apply_end LESS config_apply_start)
    message(FATAL_ERROR "could not locate apply_pending_server_config")
endif()
math(EXPR config_apply_length "${config_apply_end} - ${config_apply_start}")
string(SUBSTRING "${sdl_viewer_source}" ${config_apply_start} ${config_apply_length} config_apply_function)
require_absent("${config_apply_function}" "wd_client_stream_ownership_reset_to_tiles"
               "server-config application must reset content ownership exactly once")

read_source("src/client/content_order.cpp" content_order_source)
string(FIND "${content_order_source}" "tile_content_epoch_presented.store(0" tile_epoch_reset)
string(FIND "${content_order_source}" "video_content_epoch_presented.store(0" video_epoch_reset)
if(tile_epoch_reset EQUAL -1 OR video_epoch_reset EQUAL -1)
    message(FATAL_ERROR "a new client configuration must clear prior presentation acknowledgements")
endif()

read_source("cmake/WayDisplayTargets.cmake" targets_source)
string(FIND "${targets_source}"
       "target_link_libraries(waydisplay_client_runtime PUBLIC"
       client_runtime_links_start)
if(client_runtime_links_start EQUAL -1)
    message(FATAL_ERROR "waydisplay_client_runtime must publish its runtime link dependencies")
endif()
string(SUBSTRING "${targets_source}" ${client_runtime_links_start} 256 client_runtime_links)
foreach(required_dependency "waydisplay_common" "Threads::Threads")
    string(FIND "${client_runtime_links}" "${required_dependency}" dependency_position)
    if(dependency_position EQUAL -1)
        message(FATAL_ERROR
            "waydisplay_client_runtime must link ${required_dependency} transitively")
    endif()
endforeach()
