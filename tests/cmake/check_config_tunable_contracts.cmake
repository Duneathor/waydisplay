if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

function(read_source relative out_var)
    file(READ "${WAYDISPLAY_SOURCE_DIR}/${relative}" content)
    set(${out_var} "${content}" PARENT_SCOPE)
endfunction()

function(require_present text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${description}: missing '${pattern}'")
    endif()
endfunction()

function(require_absent text pattern description)
    string(FIND "${text}" "${pattern}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "${description}: found local policy value '${pattern}'")
    endif()
endfunction()

read_source("include/waydisplay/wd_config.h" config)
foreach(required_tunable
        WD_CLIENT_TILE_REASSEMBLY_MAX_ACTIVE_ENTRIES
        WD_CLIENT_TILE_REASSEMBLY_MAX_ACTIVE_PAYLOAD_BYTES
        WD_CLIENT_TILE_REASSEMBLY_TIMEOUT_SLACK_NS
        WD_CLIENT_TILE_REASSEMBLY_LOSS_DECAY_PERCENT
        WD_LINK_JITTER_MAX_RTT_DIVISOR
        WD_LINK_PROBE_JITTER_SPREAD_DIVISOR
        WD_SERVER_CONNECTION_RANDOM_ATTEMPTS
        WD_CLIENT_AUDIO_DECODE_QUEUE_CAPACITY
        WD_AUDIO_FRAME_DURATION_MS_DEFAULT
        WD_AUDIO_CAPTURE_CONNECT_TIMEOUT_SECONDS
        WD_AUDIO_ROUTING_PIPEWIRE_PROPS_MAX
        WD_AUDIO_ENCODER_SIGNAL_MODE
        WD_SELECTION_CAPTURE_GROWTH_MULTIPLIER
        WD_VIDEO_ENCODER_VAAPI_PROBE_WIDTH
        WD_VIDEO_ENCODER_SOFTWARE_PRESET
        WD_VIDEO_ENCODER_H265_PRIVATE_PARAMS
        WD_VIDEO_SCALER_USE_FAST_BILINEAR
        WD_CLIENT_VIDEO_DECODER_THREADS
        WD_CLIENT_RENDER_DEFAULT_PIXEL_COST_Q16
        WD_CLIENT_FRAMEBUFFER_CLEAR_XRGB
        WD_CLIENT_CONTEXT_MENU_BG_R
        WD_SERVER_READBACK_REGION_CAPACITY
        WD_SERVER_DIRTY_REGION_HEAP_INITIAL_CAPACITY
        WD_SERVER_POINTER_LOG_INTERVAL_NS
        WD_XWAYLAND_TITLEBAR_COLOR_R)
    require_present("${config}" "#define ${required_tunable}" "build-time policy must remain centralized")
endforeach()

foreach(retired_tunable
        WD_THROUGHPUT_PROBE_TARGET_BYTES
        WD_NET_MTU_PROBE_PAYLOAD_VHIGH
        WD_NET_MTU_PROBE_PAYLOAD_MHIGH
        WD_STREAM_BOOTSTRAP_SUPPRESSION_TIMEOUT_SECONDS
        WD_STREAM_VIDEO_DERIVED_BUDGET_PERCENT
        WD_TILE_SIZE_MEGA_WIDTH
        WD_TILE_SIZE_HUGE_WIDTH
        WD_TILE_AUTO_ENTRY_DIRTY_FLOOR_DIVISOR)
    require_absent("${config}" "#define ${retired_tunable}" "retired configuration must not remain as a dead knob")
endforeach()

read_source("src/client/client_net.cpp" client_net)
require_absent("${client_net}" "CLIENT_AUDIO_DECODE_QUEUE_CAPACITY = 32" "audio decode queue capacity")
require_absent("${client_net}" "jitter_ns / 4" "retransmit jitter initialization")
require_absent("${client_net}" "2u * jitter_deviation_ns" "retransmit jitter multiplier")

read_source("src/client/tile_reassembly.cpp" tile_reassembly)
foreach(forbidden
        "constexpr uint64_t TILE_REASSEMBLY_TIMEOUT_SLACK_NS"
        "constexpr size_t   MAX_RECYCLED_ENTRIES"
        "constexpr size_t   MAX_RECYCLED_COMPLETED_BUFFERS"
        "reserve(256)"
        "old_ns * 3ull / 4ull"
        "2.0 * state.tile_reassembly_deviation_ns")
    require_absent("${tile_reassembly}" "${forbidden}" "tile reassembly policy")
endforeach()

read_source("src/client/render_planning.cpp" render_planning)
require_absent("${render_planning}" "constexpr uint64_t DEFAULT_PIXEL_COST_Q16" "renderer calibration defaults")
require_absent("${render_planning}" "constexpr uint64_t DEFAULT_SNAPSHOT_PIXEL_COST_Q16" "renderer calibration defaults")

read_source("src/client/sdl_viewer.cpp" sdl_viewer)
require_absent("${sdl_viewer}" "0xff202020u" "framebuffer clear color")
require_absent("${sdl_viewer}" "udp_payload_target + 512" "UDP receive slack")
require_absent("${sdl_viewer}" "menu.x + 5" "context-menu shadow offset")

read_source("src/server/wd_server_net.c" server_net)
require_absent("${server_net}" "WD_LINK_RTT_MAX_NS / 2ull" "link jitter clamp")
require_absent("${server_net}" "(max_ns - min_ns) / 2ull" "probe jitter estimator")

read_source("src/server/wd_audio_stream.c" audio_stream)
require_absent("${audio_stream}" "WD_AUDIO_TX_QUEUE_MS + 19u" "audio packet-duration accounting")

read_source("src/server/wd_connection_identity.c" connection_identity)
require_absent("${connection_identity}" "#define WD_CONNECTION_RANDOM_ATTEMPTS" "connection token retry count")

read_source("src/server/wd_readback.c" readback)
require_absent("${readback}" "#define WD_READBACK_REGION_CAPACITY" "readback batching capacity")

read_source("src/server/wd_dirty_region_scheduler.c" dirty_scheduler)
require_absent("${dirty_scheduler}" "scheduler->heap_capacity * 2u : 16u" "dirty-region initial capacity")
require_absent("${dirty_scheduler}" "capacity * 2" "dirty-region growth policy")

read_source("src/server/wd_audio_routing.h" audio_routing)
require_absent("${audio_routing}" "#define WD_AUDIO_ROUTING_" "audio routing storage limits")

read_source("src/server/wd_audio_capture.c" audio_capture)
require_absent("${audio_capture}" "params_buffer[1024]" "PipeWire parameter buffer")
require_absent("${audio_capture}" "char rate_text[16]" "PipeWire rate text buffer")
require_absent("${audio_capture}" "ts.tv_sec += 3" "PipeWire startup timeout")

read_source("src/server/wd_audio_encoder.c" audio_encoder)
require_absent("${audio_encoder}" "OPUS_SET_VBR(1)" "Opus VBR policy")
require_absent("${audio_encoder}" "OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)" "Opus signal policy")

read_source("src/server/wd_video_encoder.c" video_encoder)
foreach(forbidden
        "\"ultrafast\""
        "\"zerolatency\""
        "repeat-headers=1:sliced-threads=1"
        "repeat-headers=1:log-level=error:pools=none:frame-threads=1"
        "max_b_frames = 0")
    require_absent("${video_encoder}" "${forbidden}" "video encoder policy")
endforeach()

read_source("src/server/wd_pointer.c" pointer)
require_absent("${pointer}" "250000000ull" "pointer debug-log interval")
require_absent("${pointer}" "last_debug_x - 24" "pointer debug-log movement threshold")

read_source("src/server/wd_xwayland.c" xwayland)
require_absent("${xwayland}" "0.14f, 0.16f, 0.18f" "Xwayland decoration colors")
