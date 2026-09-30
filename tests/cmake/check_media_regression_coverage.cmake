if(NOT DEFINED WAYDISPLAY_SOURCE_DIR)
    message(FATAL_ERROR "WAYDISPLAY_SOURCE_DIR is required")
endif()

function(require_source needle file description)
    file(READ "${WAYDISPLAY_SOURCE_DIR}/${file}" contents)
    string(FIND "${contents}" "${needle}" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "${description}: ${file} lost '${needle}'")
    endif()
endfunction()

require_source("return \"libx264\";" "tests/test_video_encoder.cpp"
               "software encoder regression must name the H.264 implementation")
require_source("return \"libx265\";" "tests/test_video_encoder.cpp"
               "software encoder regression must name the HEVC implementation")
require_source("return \"libaom-av1\";" "tests/test_video_encoder.cpp"
               "software encoder regression must name the AV1 implementation")
require_source("std::strcmp(wd_video_encoder_backend_name(encoder), expected_backend) == 0"
               "tests/test_video_encoder.cpp"
               "software encoder regression must require exact backend identity")

require_source("sampled_luma_signature" "tests/test_video_hevc_vaapi_roundtrip.cpp"
               "HEVC round-trip must inspect decoded image content")
require_source("decoded_stats.spatially_varied_frames == decoded_stats.decoded_frames"
               "tests/test_video_hevc_vaapi_roundtrip.cpp"
               "HEVC round-trip must reject blank decoded frames")
require_source("decoded_stats.signature_changes > 0" "tests/test_video_hevc_vaapi_roundtrip.cpp"
               "HEVC round-trip must observe changing decoded content")

require_source("gbm_bo_get_fd" "tests/test_video_encoder_drm_prime_vaapi.cpp"
               "DRM PRIME regression must start from a real GBM dma-buf")
require_source("wd_frame_set_drm_prime_dup" "tests/test_video_encoder_drm_prime_vaapi.cpp"
               "DRM PRIME regression must build the production frame abstraction")
require_source("wd_video_encoder_encode_frame" "tests/test_video_encoder_drm_prime_vaapi.cpp"
               "DRM PRIME regression must use the production zero-copy encoder entry point")

require_source("kStructuredFragmentCount" "tests/fuzz_tile_reassembly.cpp"
               "tile fuzzer must synthesize a multi-fragment assembly")
require_source("operation_count" "tests/fuzz_tile_reassembly.cpp"
               "tile fuzzer must execute a stateful packet sequence")
require_source("structured corpus must complete a multi-fragment tile" "tests/test_protocol_fuzz.cpp"
               "protocol fuzz regression must prove valid state is reachable")
require_source("stateful fuzz phase must still reach valid protocol packets" "tests/test_protocol_fuzz.cpp"
               "protocol fuzz regression must not devolve into an all-invalid corpus")

message(STATUS "Media regression coverage contracts satisfied")
