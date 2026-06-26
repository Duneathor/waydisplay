find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)


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
    pkg_check_modules(FFMPEG_VIDEO_ENCODER QUIET IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale
    )

    if(FFMPEG_VIDEO_ENCODER_FOUND)
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
    pkg_check_modules(FFMPEG_VIDEO_DECODER QUIET IMPORTED_TARGET
        libavcodec
        libavutil
        libswscale
    )

    if(FFMPEG_VIDEO_DECODER_FOUND)
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

function(waydisplay_find_wlroots)
    set(WAYDISPLAY_WLROOTS_MODULE "wlroots-0.19" PARENT_SCOPE)
    pkg_check_modules(WLROOTS_CANDIDATE QUIET IMPORTED_TARGET wlroots-0.19)
    set(WAYDISPLAY_WLROOTS_FOUND ${WLROOTS_CANDIDATE_FOUND} PARENT_SCOPE)
endfunction()
