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
require_absent("${video_source}" "net->video_tx && wd_video_encoder_available(net->video_encoder)"
               "video snapshot admission must use negotiated/worker state rather than racing encoder capability caches")

read_source("src/server/wd_stream_frame_worker.c" frame_worker_source)
string(FIND "${frame_worker_source}" "cpu_framebuffer_refreshed" cpu_capture_mailbox)
if(cpu_capture_mailbox EQUAL -1)
    message(FATAL_ERROR "frame-worker mailbox must preserve whether a render refreshed the CPU framebuffer")
endif()

read_source("src/server/wd_readback.c" readback_source)
require_absent("${readback_source}" ".data   = server->framebuffer_xrgb8888"
               "wlroots texture reads must target transactional scratch storage")
string(FIND "${readback_source}" "if (built_state && result != WD_RENDER_RESULT_ERROR)" capture_commit_gate)
string(FIND "${readback_source}"
       "memcpy(server->framebuffer_xrgb8888, server->framebuffer_readback_xrgb8888, server->framebuffer_bytes)"
       capture_publish)
if(capture_commit_gate EQUAL -1 OR capture_publish EQUAL -1 OR capture_publish LESS capture_commit_gate)
    message(FATAL_ERROR "CPU readback must publish scratch storage only after successful output commit")
endif()

read_source("src/server/wd_video_encoder.c" video_encoder_source)
string(FIND "${video_encoder_source}" "failed to receive encoded video packet" encoder_receive_failure)
string(FIND "${video_encoder_source}" "encoder->keyframe_requested = true;" encoder_keyframe_rearm)
if(encoder_receive_failure EQUAL -1 OR encoder_keyframe_rearm EQUAL -1)
    message(FATAL_ERROR "post-submit encoder output failures must re-arm a keyframe")
endif()

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

string(FIND "${server_net_source}" "wd_tcp_reader_destroy(&video_reader);" disconnect_start)
string(FIND "${server_net_source}" "WD_LOG_INFO(\"client disconnected; waiting for reconnect\");" disconnect_end)
if(disconnect_start EQUAL -1 OR disconnect_end EQUAL -1 OR disconnect_end LESS disconnect_start)
    message(FATAL_ERROR "could not locate client disconnect teardown")
endif()
math(EXPR disconnect_length "${disconnect_end} - ${disconnect_start}")
string(SUBSTRING "${server_net_source}" ${disconnect_start} ${disconnect_length} disconnect_teardown)
string(FIND "${disconnect_teardown}" "wd_server_rotate_session_tcp_senders" sender_rotation)
if(sender_rotation EQUAL -1)
    message(FATAL_ERROR
        "client disconnect must rotate async TCP senders so prior-session completions cannot tear down a reconnect")
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

string(FIND "${stream_source}" "static uint64_t wd_stream_tile_byte_budget_locked" tile_budget_start)
string(FIND "${stream_source}" "static void wd_stream_consume_tile_bytes_locked" tile_budget_end)
if(tile_budget_start EQUAL -1 OR tile_budget_end EQUAL -1 OR tile_budget_end LESS tile_budget_start)
    message(FATAL_ERROR "could not locate wd_stream_tile_byte_budget_locked")
endif()
math(EXPR tile_budget_length "${tile_budget_end} - ${tile_budget_start}")
string(SUBSTRING "${stream_source}" ${tile_budget_start} ${tile_budget_length} tile_budget_function)
string(FIND "${tile_budget_function}" "wd_now_ns()" tile_budget_clock)
if(tile_budget_clock EQUAL -1)
    message(FATAL_ERROR "tile-budget checks must sample the monotonic clock at the point of use")
endif()
require_absent("${tile_budget_function}" "bool repair, uint64_t"
               "tile-budget checks must not accept caller-owned timestamp snapshots")

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
string(FIND "${sdl_viewer_source}" "VideoTextureUploadResult upload_pending_video_texture" video_upload_start)
string(FIND "${sdl_viewer_source}" "struct StagedTextureRect" video_upload_end)
if(video_upload_start EQUAL -1 OR video_upload_end EQUAL -1 OR video_upload_end LESS video_upload_start)
    message(FATAL_ERROR "could not locate upload_pending_video_texture")
endif()
math(EXPR video_upload_length "${video_upload_end} - ${video_upload_start}")
string(SUBSTRING "${sdl_viewer_source}" ${video_upload_start} ${video_upload_length} video_upload_function)
string(FIND "${video_upload_function}" "state.remote_content_mutex" video_content_lock)
string(FIND "${video_upload_function}" "std::scoped_lock dirty_video_lock" video_present_lock)
if(video_content_lock EQUAL -1 OR video_present_lock EQUAL -1)
    message(FATAL_ERROR "video presentation must snapshot remote content and protect the present queue")
endif()
if(video_content_lock GREATER video_present_lock)
    message(FATAL_ERROR
        "video presentation must acquire remote-content state before dirty/video queue state to preserve the canonical lock order")
endif()

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

# Reconnects multiplex several independent TCP channels. Selection traffic
# must never share the control sender's failure domain, and local enqueue
# pressure for coalescible control traffic must not be escalated into a
# transport teardown.
string(FIND "${server_internal_source}" "struct wd_async_tcp_sender* selection_tx;" selection_sender_state)
if(selection_sender_state EQUAL -1)
    message(FATAL_ERROR "selection traffic must use a session-owned async sender distinct from control")
endif()

read_source("src/server/wd_async_tcp.h" async_tcp_header_source)
string(FIND "${async_tcp_header_source}" "wd_async_tcp_sender_transport_failures" transport_failure_api)
if(transport_failure_api EQUAL -1)
    message(FATAL_ERROR "async TCP senders must distinguish transport failures from local enqueue failures")
endif()

read_source("src/server/wd_clipboard.c" clipboard_source)
string(FIND "${clipboard_source}" "static bool send_local_selection_locked" selection_send_start)
string(FIND "${clipboard_source}" "void wd_clipboard_send_pending_locked" selection_send_end)
if(selection_send_start EQUAL -1 OR selection_send_end EQUAL -1 OR selection_send_end LESS selection_send_start)
    message(FATAL_ERROR "could not locate send_local_selection_locked")
endif()
math(EXPR selection_send_length "${selection_send_end} - ${selection_send_start}")
string(SUBSTRING "${clipboard_source}" ${selection_send_start} ${selection_send_length} selection_send_function)
string(FIND "${selection_send_function}" "net->selection_tx" selection_sender_use)
if(selection_sender_use EQUAL -1)
    message(FATAL_ERROR "clipboard selection writes must be queued on selection_tx")
endif()
require_absent("${selection_send_function}" "net->control_tx"
               "clipboard selection writes must not share the control sender")

read_source("src/server/wd_cursor.c" cursor_source)
string(FIND "${cursor_source}" "static bool wd_cursor_send_shape_locked" cursor_send_start)
string(FIND "${cursor_source}" "bool wd_cursor_flush_pending_locked" cursor_send_end)
if(cursor_send_start EQUAL -1 OR cursor_send_end EQUAL -1 OR cursor_send_end LESS cursor_send_start)
    message(FATAL_ERROR "could not locate wd_cursor_send_shape_locked")
endif()
math(EXPR cursor_send_length "${cursor_send_end} - ${cursor_send_start}")
string(SUBSTRING "${cursor_source}" ${cursor_send_start} ${cursor_send_length} cursor_send_function)
require_absent("${cursor_send_function}" "shutdown(net->tcp_fd"
               "coalescible cursor enqueue pressure must not close the control channel")

# Cursor shape is shared with the network thread during session establishment.
# Runtime mutations and request telemetry must use net.lock, and both cursor
# protocols must validate focus ownership plus a seat-client event serial.
string(FIND "${cursor_source}" "bool wd_cursor_set_shape" cursor_set_start)
string(FIND "${cursor_source}" "uint16_t wd_cursor_shape_for_resize_edges" cursor_set_end)
if(cursor_set_start EQUAL -1 OR cursor_set_end EQUAL -1 OR cursor_set_end LESS cursor_set_start)
    message(FATAL_ERROR "could not locate wd_cursor_set_shape")
endif()
math(EXPR cursor_set_length "${cursor_set_end} - ${cursor_set_start}")
string(SUBSTRING "${cursor_source}" ${cursor_set_start} ${cursor_set_length} cursor_set_function)
string(FIND "${cursor_set_function}" "pthread_mutex_lock(&server->net.lock)" cursor_state_lock)
string(FIND "${cursor_set_function}" "server->cursor_shape != shape" cursor_state_compare)
string(FIND "${cursor_set_function}" "wd_cursor_queue_current_locked(server)" cursor_state_queue)
if(cursor_state_lock EQUAL -1 OR cursor_state_compare EQUAL -1 OR cursor_state_queue EQUAL -1)
    message(FATAL_ERROR "cursor shape mutation must be serialized by net.lock")
endif()

string(FIND "${cursor_source}" "server->seat->pointer_state.focused_client == seat_client" cursor_focus_validation)
if(cursor_focus_validation EQUAL -1)
    message(FATAL_ERROR "cursor requests must require the exact pointer-focused seat client")
endif()
string(FIND "${cursor_source}" "wlr_seat_client_validate_event_serial(seat_client, serial)" cursor_serial_validation)
if(cursor_serial_validation EQUAL -1)
    message(FATAL_ERROR "cursor requests must validate their wlroots event serial")
endif()
string(FIND "${cursor_source}" "server->net.stats.cursor_shape_rejected++" cursor_shape_rejection_stat)
if(cursor_shape_rejection_stat EQUAL -1)
    message(FATAL_ERROR "cursor-shape rejection must be observable in telemetry")
endif()
string(FIND "${cursor_source}" "static void wd_cursor_record_shape_request" cursor_stat_start)
string(FIND "${cursor_source}" "static void handle_cursor_shape_request" cursor_stat_end)
if(cursor_stat_start EQUAL -1 OR cursor_stat_end EQUAL -1 OR cursor_stat_end LESS cursor_stat_start)
    message(FATAL_ERROR "could not locate cursor request telemetry helpers")
endif()
math(EXPR cursor_stat_length "${cursor_stat_end} - ${cursor_stat_start}")
string(SUBSTRING "${cursor_source}" ${cursor_stat_start} ${cursor_stat_length} cursor_stat_helpers)
string(FIND "${cursor_stat_helpers}" "pthread_mutex_lock(&server->net.lock)" cursor_stat_lock)
if(cursor_stat_lock EQUAL -1)
    message(FATAL_ERROR "cursor request telemetry must be serialized by net.lock")
endif()

string(FIND "${stream_source}" "static bool wd_stream_send_generation_summary_kind_locked" summary_send_start)
string(FIND "${stream_source}" "bool wd_stream_send_generation_summary_locked" summary_send_end)
if(summary_send_start EQUAL -1 OR summary_send_end EQUAL -1 OR summary_send_end LESS summary_send_start)
    message(FATAL_ERROR "could not locate wd_stream_send_generation_summary_kind_locked")
endif()
math(EXPR summary_send_length "${summary_send_end} - ${summary_send_start}")
string(SUBSTRING "${stream_source}" ${summary_send_start} ${summary_send_length} summary_send_function)
require_absent("${summary_send_function}" "shutdown(net->tcp_fd"
               "generation-summary enqueue pressure must not close the control channel")

string(FIND "${server_source}" "wd_async_tcp_sender_transport_failures(server->net.control_tx)" control_transport_sampling)
if(control_transport_sampling EQUAL -1)
    message(FATAL_ERROR "control teardown must be driven by transport failures, not aggregate enqueue failures")
endif()
string(FIND "${server_source}" "wd_async_tcp_sender_transport_failures(server->net.selection_tx)" selection_transport_sampling)
if(selection_transport_sampling EQUAL -1)
    message(FATAL_ERROR "selection transport failures must be sampled independently")
endif()

# TCP SQ submission failures are distinct from SEND CQE failures. Both
# implementations must preserve transient submit state for a later retry.
read_source("src/client/client_async_tcp.cpp" client_async_tcp_source)
read_source("src/server/wd_async_tcp.c" server_async_tcp_source)
foreach(async_source "${client_async_tcp_source}" "${server_async_tcp_source}")
    string(FIND "${async_source}" "wd_async_tcp_submit_result" submit_policy_use)
    string(FIND "${async_source}" "submit_retry_pending" submit_retry_state)
    if(submit_policy_use EQUAL -1 OR submit_retry_state EQUAL -1)
        message(FATAL_ERROR "async TCP sender must retain retryable io_uring_submit state")
    endif()
endforeach()

# Every reconnect-sensitive TCP channel must retain its own failure domain.
# Required input/selection RX failures intentionally end the session; optional
# video/audio transport failures must remain scoped to those media channels.
function(require_present text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${pattern}'")
    endif()
endfunction()

string(FIND "${server_source}" "if (server->net.control_tx)" control_failure_start)
string(FIND "${server_source}" "if (server->net.selection_tx)" control_failure_end)
if(control_failure_start EQUAL -1 OR control_failure_end EQUAL -1 OR control_failure_end LESS control_failure_start)
    message(FATAL_ERROR "could not isolate control async failure domain")
endif()
math(EXPR control_failure_length "${control_failure_end} - ${control_failure_start}")
string(SUBSTRING "${server_source}" ${control_failure_start} ${control_failure_length} control_failure_block)
require_present("${control_failure_block}" "shutdown(server->net.tcp_fd" "control transport failure must close control")
require_absent("${control_failure_block}" "shutdown(server->net.selection_tcp_fd" "control failure must not close selection")
require_absent("${control_failure_block}" "shutdown(server->net.video_tcp_fd" "control failure must not close video")

string(FIND "${server_source}" "if (server->net.selection_tx)" selection_failure_start)
string(FIND "${server_source}" "if (server->net.video_tx)" selection_failure_end)
if(selection_failure_start EQUAL -1 OR selection_failure_end EQUAL -1 OR selection_failure_end LESS selection_failure_start)
    message(FATAL_ERROR "could not isolate selection async failure domain")
endif()
math(EXPR selection_failure_length "${selection_failure_end} - ${selection_failure_start}")
string(SUBSTRING "${server_source}" ${selection_failure_start} ${selection_failure_length} selection_failure_block)
require_present("${selection_failure_block}" "shutdown(server->net.selection_tcp_fd" "selection transport failure must close selection")
require_absent("${selection_failure_block}" "shutdown(server->net.tcp_fd" "selection failure must not close control")
require_absent("${selection_failure_block}" "shutdown(server->net.video_tcp_fd" "selection failure must not close video")

string(FIND "${server_source}" "if (server->net.video_tx)" video_failure_start)
string(FIND "${server_source}" "if (server->net.udp_tx)" video_failure_end)
if(video_failure_start EQUAL -1 OR video_failure_end EQUAL -1 OR video_failure_end LESS video_failure_start)
    message(FATAL_ERROR "could not isolate video async failure domain")
endif()
math(EXPR video_failure_length "${video_failure_end} - ${video_failure_start}")
string(SUBSTRING "${server_source}" ${video_failure_start} ${video_failure_length} video_failure_block)
require_present("${video_failure_block}" "shutdown(server->net.video_tcp_fd" "video transport failure must close video")
require_present("${video_failure_block}" "wd_stream_video_reset_locked" "video transport failure must return ownership through video reset")
require_absent("${video_failure_block}" "shutdown(server->net.tcp_fd" "video failure must not close control")
require_absent("${video_failure_block}" "shutdown(server->net.selection_tcp_fd" "video failure must not close selection")

string(FIND "${server_net_source}" "if (have_input_pfd" input_rx_start)
string(FIND "${server_net_source}" "if (have_select_pfd" input_rx_end)
if(input_rx_start EQUAL -1 OR input_rx_end EQUAL -1 OR input_rx_end LESS input_rx_start)
    message(FATAL_ERROR "could not isolate input receive failure domain")
endif()
math(EXPR input_rx_length "${input_rx_end} - ${input_rx_start}")
string(SUBSTRING "${server_net_source}" ${input_rx_start} ${input_rx_length} input_rx_block)
require_present("${input_rx_block}" "close(input_tcp_fd)" "input RX failure must close input")
require_present("${input_rx_block}" "break;" "required input RX failure must end the session")
require_absent("${input_rx_block}" "close(selection_tcp_fd)" "input RX failure must not directly close selection")

string(FIND "${server_net_source}" "if (have_select_pfd" selection_rx_start)
string(FIND "${server_net_source}" "if (have_video_pfd" selection_rx_end)
if(selection_rx_start EQUAL -1 OR selection_rx_end EQUAL -1 OR selection_rx_end LESS selection_rx_start)
    message(FATAL_ERROR "could not isolate selection receive failure domain")
endif()
math(EXPR selection_rx_length "${selection_rx_end} - ${selection_rx_start}")
string(SUBSTRING "${server_net_source}" ${selection_rx_start} ${selection_rx_length} selection_rx_block)
require_present("${selection_rx_block}" "close(selection_tcp_fd)" "selection RX failure must close selection")
require_present("${selection_rx_block}" "break;" "required selection RX failure must end the session")
require_absent("${selection_rx_block}" "close(input_tcp_fd)" "selection RX failure must not directly close input")

read_source("src/server/wd_audio_stream.c" audio_stream_source)
string(FIND "${audio_stream_source}" "static void* wd_audio_stream_worker" audio_worker_start)
string(FIND "${audio_stream_source}" "bool wd_audio_stream_create" audio_worker_end)
if(audio_worker_start EQUAL -1 OR audio_worker_end EQUAL -1 OR audio_worker_end LESS audio_worker_start)
    message(FATAL_ERROR "could not isolate audio stream worker")
endif()
math(EXPR audio_worker_length "${audio_worker_end} - ${audio_worker_start}")
string(SUBSTRING "${audio_stream_source}" ${audio_worker_start} ${audio_worker_length} audio_worker_block)
require_present("${audio_worker_block}" "shutdown(stream->tcp_fd" "audio failure must close its audio transport")
require_absent("${audio_worker_block}" "net->tcp_fd" "audio worker must not own the control transport")

require_absent("${audio_stream_source}" "memset(&stream->stats" "audio statistics reset must remain atomic")
require_present("${audio_stream_source}" "errno == EINTR" "audio worker sleep must retry only interrupted nanosleep calls")
require_present("${audio_stream_source}" "char sink_name[WD_AUDIO_ROUTING_SINK_MAX]" "audio routing identity must be stream-owned")
require_present("${audio_stream_source}" "bool wd_audio_stream_prepare" "audio capture recreation must be explicit")
string(FIND "${audio_stream_source}" "bool wd_audio_stream_ready" audio_ready_start)
string(FIND "${audio_stream_source}" "const char* wd_audio_stream_sink_name" audio_ready_end)
if(audio_ready_start EQUAL -1 OR audio_ready_end EQUAL -1 OR audio_ready_end LESS audio_ready_start)
    message(FATAL_ERROR "could not isolate pure audio readiness probe")
endif()
math(EXPR audio_ready_length "${audio_ready_end} - ${audio_ready_start}")
string(SUBSTRING "${audio_stream_source}" ${audio_ready_start} ${audio_ready_length} audio_ready_block)
require_absent("${audio_ready_block}" "wd_audio_stream_ensure_capture_locked" "audio readiness probe must not recreate capture")

read_source("src/client/client_state.hpp" client_state_source)
string(FIND "${client_state_source}" "async_udp_stats_mutex" udp_stats_mutex)
if(udp_stats_mutex EQUAL -1)
    message(FATAL_ERROR "client UDP async delta cursors must have a dedicated stats mutex")
endif()

string(FIND "${server_net_source}" "pthread_mutex_lock(&net->lock);\n    wd_net_derive_link_profile" link_profile_lock)
if(link_profile_lock EQUAL -1)
    message(FATAL_ERROR "link-profile publication must occur under net.lock")
endif()

string(FIND "${stream_source}" "tile_input_inject_ns" tile_input_pair)
string(FIND "${stream_source}" "summary_input_sequence == completion->input_sequence" summary_generation_guard)
if(tile_input_pair EQUAL -1 OR summary_generation_guard EQUAL -1)
    message(FATAL_ERROR "input latency telemetry must carry sequence/timestamp pairs across async work")
endif()

require_absent("${telemetry_source}" "dst->tcp_summary_budget_interval_ns +="
               "summary interval telemetry is a gauge and must not be accumulated")
require_absent("${telemetry_source}" "wire_mbit_per_sec"
               "server video throughput is payload throughput, not on-wire bitrate")
read_source("src/client/client_telemetry.cpp" client_telemetry_source)
require_absent("${client_telemetry_source}" "wire_mbit_per_sec"
               "client video throughput is payload throughput, not on-wire bitrate")
require_absent("${client_telemetry_source}" "erase(state.recent_input_timestamps.begin(), std::next(it))"
               "presentation timestamp lookup must erase only the matched sequence")
read_source("src/client/sdl_viewer.cpp" sdl_telemetry_source)
require_absent("${sdl_telemetry_source}" "sdl_texture_coalesced_dirty_rects"
               "render telemetry must not claim a nonexistent second coalescing pass")

read_source("src/server/wd_keyboard_shortcuts_inhibit.c" shortcuts_source)
string(FIND "${shortcuts_source}" "void wd_keyboard_shortcuts_inhibit_refresh" shortcuts_refresh_start)
string(FIND "${shortcuts_source}" "bool wd_keyboard_shortcuts_inhibit_active" shortcuts_refresh_end)
if(shortcuts_refresh_start EQUAL -1 OR shortcuts_refresh_end EQUAL -1 OR shortcuts_refresh_end LESS shortcuts_refresh_start)
    message(FATAL_ERROR "could not isolate keyboard-shortcuts-inhibit refresh")
endif()
math(EXPR shortcuts_refresh_length "${shortcuts_refresh_end} - ${shortcuts_refresh_start}")
string(SUBSTRING "${shortcuts_source}" ${shortcuts_refresh_start} ${shortcuts_refresh_length} shortcuts_refresh_block)
require_absent("${shortcuts_refresh_block}" "wlr_keyboard_shortcuts_inhibitor_v1_deactivate"
               "focus loss must not emit protocol inactive for a live shortcuts inhibitor")
require_present("${shortcuts_source}" "state->inhibitor->active && inhibitor_should_be_active"
                "shortcut suppression relevance must combine protocol activation with current focus")
