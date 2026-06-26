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
