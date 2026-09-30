#include "wd_video_encoder.h"
#include "wd_hevc_annexb.h"
#include "video_av1_tiles.h"
#include "video_vaapi_quality_policy.h"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WAYDISPLAY_HAVE_H265_SERVER_ENCODER
#define WAYDISPLAY_HAVE_H265_SERVER_ENCODER 0
#endif

#ifndef WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
#define WAYDISPLAY_HAVE_AV1_SERVER_ENCODER 0
#endif

#ifndef WAYDISPLAY_HAVE_H264_SERVER_ENCODER
#define WAYDISPLAY_HAVE_H264_SERVER_ENCODER 0
#endif

#ifndef WAYDISPLAY_HAVE_VAAPI_SERVER_PROFILE_CHECK
#define WAYDISPLAY_HAVE_VAAPI_SERVER_PROFILE_CHECK 0
#endif

#ifndef WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
#define WAYDISPLAY_HAVE_VAAPI_SERVER_VPP 0
#endif

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
enum {
    /*
     * Use a codec-block-aligned probe frame large enough for drivers whose
     * minimum encode width is greater than 128 pixels (for example 130).
     * This is a one-time capability probe, so the extra surface size is
     * negligible and avoids false negatives from an undersized test frame.
     */
    WD_VAAPI_PROBE_WIDTH      = WD_VIDEO_ENCODER_VAAPI_PROBE_WIDTH,
    WD_VAAPI_PROBE_HEIGHT     = WD_VIDEO_ENCODER_VAAPI_PROBE_HEIGHT,
    WD_FFMPEG_FRAME_ALIGNMENT = WD_VIDEO_ENCODER_FFMPEG_FRAME_ALIGNMENT,
};
#include "../common/wd_vaapi_device.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if WAYDISPLAY_HAVE_VAAPI_SERVER_PROFILE_CHECK || WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#endif
#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
#include <sys/stat.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#endif
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#endif

enum wd_video_encoder_preference {
    WD_VIDEO_ENCODER_PREFERENCE_AUTO = 0,
    WD_VIDEO_ENCODER_PREFERENCE_OFF,
    WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE,
    WD_VIDEO_ENCODER_PREFERENCE_VAAPI,
};

enum wd_video_encoder_backend {
    WD_VIDEO_ENCODER_BACKEND_NONE = 0,
    WD_VIDEO_ENCODER_BACKEND_SOFTWARE,
    WD_VIDEO_ENCODER_BACKEND_VAAPI,
};

struct wd_video_encoder {
    struct wd_video_encoder_config   config;
    bool                             configured;
    bool                             keyframe_requested;
    uint64_t                         next_frame_id;
    enum wd_video_encoder_preference preference;
    enum wd_video_encoder_backend    active_backend;
    char                             vaapi_device[PATH_MAX];
    uint32_t                         vaapi_failed_codecs;

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    const AVCodec*     codec;
    AVCodecContext*    codec_ctx;
    AVFrame*           frame;
    AVFrame*           upload_frame;
    AVPacket*          packet;
    struct SwsContext* sws_ctx;
    AVBufferRef*       vaapi_device_ctx;
    AVBufferRef*       vaapi_frames_ctx;
    bool               vaapi_probe_complete;
    uint32_t           vaapi_supported_codecs;
#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
    VAConfigID         vaapi_vpp_config;
    VAContextID        vaapi_vpp_context;
    bool               vaapi_vpp_ready;
#endif
    uint32_t*          padded_pixels;
    size_t             padded_pixel_capacity;
#endif
};

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
static const char* wd_video_encoder_codec_name(uint32_t codec) {
    switch (codec)
    {
    case WD_VIDEO_CODEC_H264:
        return "h264";
    case WD_VIDEO_CODEC_H265:
        return "h265";
    case WD_VIDEO_CODEC_AV1:
        return "av1";
    default:
        return "unknown";
    }
}

static void wd_video_encoder_log_av_error(const char* action, int error_code) {
    char error_text[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(error_code, error_text, sizeof(error_text)) < 0)
    {
        snprintf(error_text, sizeof(error_text), "FFmpeg error %d", error_code);
    }
    WD_LOG_WARN("%s: %s", action, error_text);
}

static void wd_video_encoder_release_backend(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return;
    }

#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
    if (encoder->vaapi_vpp_ready && encoder->vaapi_device_ctx)
    {
        AVHWDeviceContext* hwdev = (AVHWDeviceContext*)encoder->vaapi_device_ctx->data;
        AVVAAPIDeviceContext* va = hwdev ? (AVVAAPIDeviceContext*)hwdev->hwctx : NULL;
        if (va && va->display)
        {
            if (encoder->vaapi_vpp_context != VA_INVALID_ID)
            {
                (void)vaDestroyContext(va->display, encoder->vaapi_vpp_context);
            }
            if (encoder->vaapi_vpp_config != VA_INVALID_ID)
            {
                (void)vaDestroyConfig(va->display, encoder->vaapi_vpp_config);
            }
        }
    }
    encoder->vaapi_vpp_config  = VA_INVALID_ID;
    encoder->vaapi_vpp_context = VA_INVALID_ID;
    encoder->vaapi_vpp_ready   = false;
#endif

    sws_freeContext(encoder->sws_ctx);
    encoder->sws_ctx = NULL;

    free(encoder->padded_pixels);
    encoder->padded_pixels         = NULL;
    encoder->padded_pixel_capacity = 0;

    av_packet_free(&encoder->packet);
    av_frame_free(&encoder->upload_frame);
    av_frame_free(&encoder->frame);
    avcodec_free_context(&encoder->codec_ctx);
    av_buffer_unref(&encoder->vaapi_frames_ctx);

    encoder->codec          = NULL;
    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_NONE;
}

static const AVCodec* wd_video_encoder_find_software_codec(uint32_t codec) {
    switch (codec)
    {
#if WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    case WD_VIDEO_CODEC_H264:
        return avcodec_find_encoder_by_name("libx264");
#endif
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    case WD_VIDEO_CODEC_H265:
        return avcodec_find_encoder_by_name("libx265");
#endif
#if WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    case WD_VIDEO_CODEC_AV1:
        return avcodec_find_encoder_by_name("libaom-av1");
#endif
    default:
        return NULL;
    }
}

static const AVCodec* wd_video_encoder_find_vaapi_codec(uint32_t codec) {
    switch (codec)
    {
#if WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    case WD_VIDEO_CODEC_H264:
        return avcodec_find_encoder_by_name("h264_vaapi");
#endif
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    case WD_VIDEO_CODEC_H265:
        return avcodec_find_encoder_by_name("hevc_vaapi");
#endif
#if WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    case WD_VIDEO_CODEC_AV1:
        return avcodec_find_encoder_by_name("av1_vaapi");
#endif
    default:
        return NULL;
    }
}

static bool wd_video_encoder_probe_vaapi_config_on_device(struct wd_video_encoder* encoder, AVBufferRef* device,
                                                          const struct wd_video_encoder_config* config);
static bool wd_video_encoder_vaapi_codec_available(struct wd_video_encoder* encoder, uint32_t codec);
static bool wd_video_encoder_select_vaapi_device_for_config(struct wd_video_encoder* encoder,
                                                            const struct wd_video_encoder_config* config);

static uint32_t wd_video_encoder_detect_software_codecs(void) {
    uint32_t codecs = 0;
#if WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    if (wd_video_encoder_find_software_codec(WD_VIDEO_CODEC_H264))
    {
        codecs |= WD_VIDEO_CODEC_H264;
    }
#endif
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (wd_video_encoder_find_software_codec(WD_VIDEO_CODEC_H265))
    {
        codecs |= WD_VIDEO_CODEC_H265;
    }
#endif
#if WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    if (wd_video_encoder_find_software_codec(WD_VIDEO_CODEC_AV1))
    {
        codecs |= WD_VIDEO_CODEC_AV1;
    }
#endif
    return codecs;
}

static uint32_t wd_video_encoder_detect_vaapi_codecs(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return 0;
    }
    if (encoder->vaapi_probe_complete)
    {
        return encoder->vaapi_supported_codecs;
    }

    encoder->vaapi_probe_complete   = true;
    encoder->vaapi_supported_codecs = 0;
#if WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    if (wd_video_encoder_find_vaapi_codec(WD_VIDEO_CODEC_H264) &&
        wd_video_encoder_vaapi_codec_available(encoder, WD_VIDEO_CODEC_H264))
    {
        encoder->vaapi_supported_codecs |= WD_VIDEO_CODEC_H264;
    }
#endif
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (wd_video_encoder_find_vaapi_codec(WD_VIDEO_CODEC_H265) &&
        wd_video_encoder_vaapi_codec_available(encoder, WD_VIDEO_CODEC_H265))
    {
        encoder->vaapi_supported_codecs |= WD_VIDEO_CODEC_H265;
    }
#endif
#if WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    if (wd_video_encoder_find_vaapi_codec(WD_VIDEO_CODEC_AV1) &&
        wd_video_encoder_vaapi_codec_available(encoder, WD_VIDEO_CODEC_AV1))
    {
        encoder->vaapi_supported_codecs |= WD_VIDEO_CODEC_AV1;
    }
#endif

    WD_LOG_DEBUG("VAAPI video encode codecs across devices: h264=%s h265=%s av1=%s",
                 (encoder->vaapi_supported_codecs & WD_VIDEO_CODEC_H264) != 0 ? "yes" : "no",
                 (encoder->vaapi_supported_codecs & WD_VIDEO_CODEC_H265) != 0 ? "yes" : "no",
                 (encoder->vaapi_supported_codecs & WD_VIDEO_CODEC_AV1) != 0 ? "yes" : "no");
    return encoder->vaapi_supported_codecs;
}

static uint32_t wd_video_encoder_detect_supported_codecs(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return 0;
    }

    switch (encoder->preference)
    {
    case WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE:
        return wd_video_encoder_detect_software_codecs();
    case WD_VIDEO_ENCODER_PREFERENCE_VAAPI:
        return wd_video_encoder_detect_vaapi_codecs(encoder) & ~encoder->vaapi_failed_codecs;
    case WD_VIDEO_ENCODER_PREFERENCE_AUTO:
    default:
        return wd_video_encoder_detect_software_codecs() |
               (wd_video_encoder_detect_vaapi_codecs(encoder) & ~encoder->vaapi_failed_codecs);
    }
}

static bool wd_video_encoder_config_matches(const struct wd_video_encoder* encoder, const struct wd_video_encoder_config* config) {
    return encoder && config && encoder->configured && encoder->codec_ctx && encoder->config.session_id == config->session_id &&
           encoder->config.connection_token == config->connection_token && encoder->config.content_epoch == config->content_epoch &&
           encoder->config.width == config->width && encoder->config.height == config->height &&
           encoder->config.target_fps == config->target_fps && encoder->config.bitrate_kib_per_second == config->bitrate_kib_per_second &&
           encoder->config.codec == config->codec;
}

static int wd_video_encoder_effective_fps(const struct wd_video_encoder_config* config) {
    if (!config || config->target_fps == 0)
    {
        return WD_VIDEO_ENCODER_FALLBACK_FPS;
    }

    return config->target_fps;
}

static int64_t wd_video_encoder_bitrate_bits_per_second(const struct wd_video_encoder_config* config) {
    if (!config || config->bitrate_kib_per_second == 0)
    {
        return (int64_t)WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND * 1024ll * 8ll;
    }

    const int64_t kib = config->bitrate_kib_per_second;
    if (kib > INT64_MAX / (1024ll * 8ll))
    {
        return INT64_MAX;
    }

    return kib * 1024ll * 8ll;
}

static int wd_video_encoder_even_dimension(uint16_t value) {
    int dimension = value;
    if ((dimension & 1) != 0)
    {
        dimension++;
    }
    return dimension;
}

static void wd_video_encoder_set_context_defaults(AVCodecContext* codec_ctx, const AVCodec* codec,
                                                  const struct wd_video_encoder_config* config, enum AVPixelFormat pixel_format) {
    const int     fps     = wd_video_encoder_effective_fps(config);
    const int64_t bitrate = wd_video_encoder_bitrate_bits_per_second(config);

    codec_ctx->codec_type   = AVMEDIA_TYPE_VIDEO;
    codec_ctx->codec_id     = codec ? codec->id : AV_CODEC_ID_NONE;
    codec_ctx->width        = wd_video_encoder_even_dimension(config->width);
    codec_ctx->height       = wd_video_encoder_even_dimension(config->height);
    codec_ctx->pix_fmt      = pixel_format;
    codec_ctx->time_base    = (AVRational){1, 1000000};
    codec_ctx->framerate    = (AVRational){fps, 1};
    codec_ctx->gop_size     = (fps > 0 ? fps : (int)WD_VIDEO_ENCODER_FALLBACK_FPS) * WD_VIDEO_ENCODER_GOP_SECONDS;
    codec_ctx->max_b_frames = WD_VIDEO_ENCODER_MAX_B_FRAMES;
    codec_ctx->bit_rate     = bitrate;
}

#if WAYDISPLAY_HAVE_AV1_SERVER_ENCODER && WAYDISPLAY_HAVE_VAAPI_SERVER_PROFILE_CHECK
/* AV1 Profile 0 decoding (VAEntrypointVLD) is not AV1 encoding. Check both
 * the profile and an encode entrypoint before asking FFmpeg to open av1_vaapi;
 * the FFmpeg probe still decides whether encoding actually works. */
static bool wd_video_encoder_vaapi_can_encode_av1(const AVBufferRef* device) {
    if (!device || !device->data)
    {
        return false;
    }

    const AVHWDeviceContext* hw_device = (const AVHWDeviceContext*)device->data;
    const AVVAAPIDeviceContext* va_device = hw_device->hwctx;
    const VADisplay display = va_device ? va_device->display : NULL;
    if (!display)
    {
        return false;
    }

    const int profile_capacity = vaMaxNumProfiles(display);
    if (profile_capacity <= 0)
    {
        return false;
    }
    VAProfile* profiles = calloc((size_t)profile_capacity, sizeof(*profiles));
    if (!profiles)
    {
        return false;
    }

    int profile_count = 0;
    const bool queried_profiles = vaQueryConfigProfiles(display, profiles, &profile_count) == VA_STATUS_SUCCESS &&
                                  profile_count >= 0 && profile_count <= profile_capacity;
    bool has_profile = false;
    if (queried_profiles)
    {
        for (int i = 0; i < profile_count; ++i)
        {
            if (profiles[i] == VAProfileAV1Profile0)
            {
                has_profile = true;
                break;
            }
        }
    }
    free(profiles);
    if (!has_profile)
    {
        return false;
    }

    const int entrypoint_capacity = vaMaxNumEntrypoints(display);
    if (entrypoint_capacity <= 0)
    {
        return false;
    }
    VAEntrypoint* entrypoints = calloc((size_t)entrypoint_capacity, sizeof(*entrypoints));
    if (!entrypoints)
    {
        return false;
    }

    int entrypoint_count = 0;
    const bool queried_entrypoints = vaQueryConfigEntrypoints(display, VAProfileAV1Profile0, entrypoints, &entrypoint_count) == VA_STATUS_SUCCESS &&
                                     entrypoint_count >= 0 && entrypoint_count <= entrypoint_capacity;
    bool can_encode = false;
    if (queried_entrypoints)
    {
        for (int i = 0; i < entrypoint_count; ++i)
        {
            if (entrypoints[i] == VAEntrypointEncSlice || entrypoints[i] == VAEntrypointEncSliceLP)
            {
                can_encode = true;
                break;
            }
        }
    }
    free(entrypoints);
    return can_encode;
}
#endif

static bool wd_video_encoder_probe_vaapi_encode_path(AVCodecContext* codec_ctx, AVBufferRef* frames_ref) {
    if (!codec_ctx || !frames_ref)
    {
        return false;
    }

    AVFrame*           upload_frame = av_frame_alloc();
    AVFrame*           vaapi_frame  = av_frame_alloc();
    AVPacket*          packet       = av_packet_alloc();
    struct SwsContext* sws_ctx      = NULL;
    uint32_t*          xrgb_pixels  = NULL;
    bool               supported    = false;
    if (!upload_frame || !vaapi_frame || !packet)
    {
        goto done;
    }

    upload_frame->format = AV_PIX_FMT_NV12;
    upload_frame->width  = codec_ctx->width;
    upload_frame->height = codec_ctx->height;
    if (av_frame_get_buffer(upload_frame, WD_FFMPEG_FRAME_ALIGNMENT) < 0 ||
        av_frame_make_writable(upload_frame) < 0)
    {
        goto done;
    }

    const size_t pixel_count = (size_t)codec_ctx->width * (size_t)codec_ctx->height;
    if (pixel_count == 0 || pixel_count > SIZE_MAX / sizeof(*xrgb_pixels))
    {
        goto done;
    }
    xrgb_pixels = malloc(pixel_count * sizeof(*xrgb_pixels));
    if (!xrgb_pixels)
    {
        goto done;
    }
    for (int y = 0; y < codec_ctx->height; ++y)
    {
        for (int x = 0; x < codec_ctx->width; ++x)
        {
            const uint32_t red   = (uint32_t)(x * 3 + y) & 0xffu;
            const uint32_t green = (uint32_t)(y * 5 + x) & 0xffu;
            const uint32_t blue  = (uint32_t)(x ^ y) & 0xffu;
            xrgb_pixels[(size_t)y * (size_t)codec_ctx->width + (size_t)x] =
                UINT32_C(0xff000000) | (red << 16u) | (green << 8u) | blue;
        }
    }

    const int scaler_flags = WD_VIDEO_SCALER_USE_FAST_BILINEAR ? SWS_FAST_BILINEAR : SWS_BILINEAR;
    sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, AV_PIX_FMT_BGRA,
                             codec_ctx->width, codec_ctx->height, AV_PIX_FMT_NV12,
                             scaler_flags, NULL, NULL, NULL);
    if (!sws_ctx)
    {
        goto done;
    }
    const uint8_t* src_slices[4] = {(const uint8_t*)xrgb_pixels, NULL, NULL, NULL};
    const int      src_stride[4] = {codec_ctx->width * (int)WD_BYTES_PER_PIXEL, 0, 0, 0};
    if (sws_scale(sws_ctx, src_slices, src_stride, 0, codec_ctx->height,
                  upload_frame->data, upload_frame->linesize) != upload_frame->height)
    {
        goto done;
    }

    /* Exercise the exact XRGB -> software-NV12 -> VAAPI upload used by the
     * runtime. Merely opening a VAAPI codec (or uploading a hand-filled NV12
     * frame) can miss driver/rate-control/packed-header failures that only
     * appear on the first real WayDisplay frame. */
    if (av_hwframe_get_buffer(frames_ref, vaapi_frame, 0) < 0 ||
        av_hwframe_transfer_data(vaapi_frame, upload_frame, 0) < 0)
    {
        goto done;
    }

    vaapi_frame->pts       = 1000000;
    vaapi_frame->pict_type = AV_PICTURE_TYPE_I;
    if (avcodec_send_frame(codec_ctx, vaapi_frame) < 0)
    {
        goto done;
    }

    /* A hardware encoder may buffer the first frame even with B-frames
     * disabled. Flush this disposable probe context so deferred driver errors
     * are observed and require at least one real encoded packet. */
    bool flush_requested = false;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        av_packet_unref(packet);
        const int rc = avcodec_receive_packet(codec_ctx, packet);
        if (rc == 0)
        {
            if (packet->size > 0)
            {
                supported = true;
                break;
            }
            continue;
        }
        if (rc == AVERROR(EAGAIN) && !flush_requested)
        {
            const int flush_rc = avcodec_send_frame(codec_ctx, NULL);
            if (flush_rc < 0 && flush_rc != AVERROR(EAGAIN) && flush_rc != AVERROR_EOF)
            {
                break;
            }
            flush_requested = true;
            continue;
        }
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
        {
            break;
        }
        break;
    }

done:
    sws_freeContext(sws_ctx);
    free(xrgb_pixels);
    av_packet_free(&packet);
    av_frame_free(&vaapi_frame);
    av_frame_free(&upload_frame);
    return supported;
}

static bool wd_video_encoder_probe_vaapi_config_on_device_once(struct wd_video_encoder* encoder, AVBufferRef* device,
                                                               const struct wd_video_encoder_config* config,
                                                               bool quality_mode) {
    if (!encoder || !device || !config)
    {
        return false;
    }

    const AVCodec* codec = wd_video_encoder_find_vaapi_codec(config->codec);
    if (!codec)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_AV1_SERVER_ENCODER && WAYDISPLAY_HAVE_VAAPI_SERVER_PROFILE_CHECK
    if (config->codec == WD_VIDEO_CODEC_AV1 && !wd_video_encoder_vaapi_can_encode_av1(device))
    {
        return false;
    }
#endif

    AVCodecContext* codec_ctx  = avcodec_alloc_context3(codec);
    AVBufferRef*    frames_ref = NULL;
    bool            supported  = false;
    if (!codec_ctx)
    {
        return false;
    }

    wd_video_encoder_set_context_defaults(codec_ctx, codec, config, AV_PIX_FMT_VAAPI);
    frames_ref = av_hwframe_ctx_alloc(device);
    if (!frames_ref)
    {
        goto done;
    }

    AVHWFramesContext* frames = (AVHWFramesContext*)frames_ref->data;
    frames->format            = AV_PIX_FMT_VAAPI;
    frames->sw_format         = AV_PIX_FMT_NV12;
    frames->width             = codec_ctx->width;
    frames->height            = codec_ctx->height;
    frames->initial_pool_size = WD_VIDEO_ENCODER_VAAPI_FRAME_POOL_SIZE;
    if (av_hwframe_ctx_init(frames_ref) < 0)
    {
        goto done;
    }

    codec_ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
    if (!codec_ctx->hw_frames_ctx)
    {
        goto done;
    }
    if (codec_ctx->priv_data)
    {
        (void)av_opt_set(codec_ctx->priv_data, "async_depth", WD_VIDEO_ENCODER_VAAPI_ASYNC_DEPTH, 0);
        if (quality_mode)
        {
            codec_ctx->bit_rate = 0;
            if (av_opt_set(codec_ctx->priv_data, "rc_mode", "CQP", 0) < 0 ||
                av_opt_set_int(codec_ctx->priv_data, "qp", WD_VIDEO_HEVC_VAAPI_HIGH_BANDWIDTH_QP, 0) < 0)
            {
                goto done;
            }
        }
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
    {
        goto done;
    }

    supported = wd_video_encoder_probe_vaapi_encode_path(codec_ctx, frames_ref);

done:
    avcodec_free_context(&codec_ctx);
    av_buffer_unref(&frames_ref);
    return supported;
}

static bool wd_video_encoder_probe_vaapi_config_on_device(struct wd_video_encoder* encoder, AVBufferRef* device,
                                                          const struct wd_video_encoder_config* config) {
    if (!encoder || !device || !config)
    {
        return false;
    }
    const bool prefer_quality = wd_video_hevc_vaapi_prefer_quality(config->codec, config->bitrate_kib_per_second);
    if (wd_video_encoder_probe_vaapi_config_on_device_once(encoder, device, config, prefer_quality))
    {
        return true;
    }
    return prefer_quality && wd_video_encoder_probe_vaapi_config_on_device_once(encoder, device, config, false);
}

struct wd_video_encoder_vaapi_match {
    struct wd_video_encoder* encoder;
    const struct wd_video_encoder_config* config;
};

static bool wd_video_encoder_vaapi_device_matches(const AVBufferRef* device, void* userdata) {
    struct wd_video_encoder_vaapi_match* match = userdata;
    return match && match->encoder && match->config &&
           wd_video_encoder_probe_vaapi_config_on_device(match->encoder, (AVBufferRef*)device, match->config);
}

static bool wd_video_encoder_vaapi_codec_available(struct wd_video_encoder* encoder, uint32_t codec) {
    if (!encoder)
    {
        return false;
    }
    const struct wd_video_encoder_config probe_config = {
        .width                  = WD_VAAPI_PROBE_WIDTH,
        .height                 = WD_VAAPI_PROBE_HEIGHT,
        .target_fps             = WD_VIDEO_ENCODER_VAAPI_PROBE_FPS,
        .bitrate_kib_per_second = WD_VIDEO_ENCODER_VAAPI_PROBE_BITRATE_KIB,
        .codec                  = codec,
    };
    struct wd_video_encoder_vaapi_match match = {.encoder = encoder, .config = &probe_config};
    AVBufferRef* device = NULL;
    const int rc = wd_vaapi_open_matching_device(&device, NULL, 0, wd_video_encoder_vaapi_device_matches, &match);
    av_buffer_unref(&device);
    return rc >= 0;
}

static bool wd_video_encoder_select_vaapi_device_for_config(struct wd_video_encoder* encoder,
                                                            const struct wd_video_encoder_config* config) {
    if (!encoder || !config)
    {
        return false;
    }
    struct wd_video_encoder_vaapi_match match = {.encoder = encoder, .config = config};
    AVBufferRef* selected = NULL;
    char selected_path[PATH_MAX] = {0};
    const int rc = wd_vaapi_open_matching_device(&selected, selected_path, sizeof(selected_path),
                                                  wd_video_encoder_vaapi_device_matches, &match);
    if (rc < 0)
    {
        wd_video_encoder_log_av_error("failed to discover a codec-capable VAAPI encode device", rc);
        av_buffer_unref(&selected);
        return false;
    }

    av_buffer_unref(&encoder->vaapi_device_ctx);
    encoder->vaapi_device_ctx = selected;
    (void)snprintf(encoder->vaapi_device, sizeof(encoder->vaapi_device), "%s", selected_path);
    WD_LOG_DEBUG("VAAPI %s encode device selected: %s", wd_video_encoder_codec_name(config->codec), encoder->vaapi_device);
    return true;
}

static bool wd_video_encoder_allocate_common(struct wd_video_encoder* encoder, const AVCodec* codec,
                                             const struct wd_video_encoder_config* config, enum AVPixelFormat pixel_format) {
    encoder->codec     = codec;
    encoder->codec_ctx = avcodec_alloc_context3(codec);
    encoder->frame     = av_frame_alloc();
    encoder->packet    = av_packet_alloc();
    if (!encoder->codec_ctx || !encoder->frame || !encoder->packet)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    wd_video_encoder_set_context_defaults(encoder->codec_ctx, codec, config, pixel_format);
    return true;
}

static bool wd_video_encoder_configure_software(struct wd_video_encoder* encoder, const struct wd_video_encoder_config* config) {
    const AVCodec* codec = wd_video_encoder_find_software_codec(config->codec);
    if (!codec || !wd_video_encoder_allocate_common(encoder, codec, config, AV_PIX_FMT_YUV420P))
    {
        return false;
    }

    encoder->codec_ctx->thread_count = WD_VIDEO_ENCODER_SOFTWARE_THREADS;
    if (config->codec == WD_VIDEO_CODEC_AV1 && encoder->codec_ctx->priv_data)
    {
        /* libaom realtime: zero lookahead so queued frames can be decoded immediately. */
        (void)av_opt_set(encoder->codec_ctx->priv_data, "usage", "realtime", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "cpu-used", "8", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "lag-in-frames", "0", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "row-mt", "1", 0);
        /* `cpu-used=8` is the maximum accepted by FFmpeg's libaom wrapper.
         * At desktop resolutions, split the frame into tiles so row-mt can
         * use multiple encoder threads; keep tiny fixtures single-tile.
         * Old libaom builds that reject the tiling option retain the
         * existing defaults rather than losing software AV1 entirely. */
        const char* tiles = wd_video_av1_software_tiles(config->width, config->height);
        const int tiles_rc = av_opt_set(encoder->codec_ctx->priv_data, "tiles", tiles, 0);
        if (tiles_rc < 0)
        {
            WD_LOG_DEBUG("AV1 software tile layout %s unavailable; using libaom default", tiles);
        }
        else
        {
            WD_LOG_DEBUG("AV1 software encoder: realtime cpu-used=8 threads=%u tiles=%s row-mt=1", WD_VIDEO_ENCODER_SOFTWARE_THREADS, tiles);
        }
    }
    else if (encoder->codec_ctx->priv_data)
    {
        (void)av_opt_set(encoder->codec_ctx->priv_data, "preset", WD_VIDEO_ENCODER_SOFTWARE_PRESET, 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "tune", WD_VIDEO_ENCODER_SOFTWARE_TUNE, 0);
        /* AVFrame.pict_type requests an intra picture.  libx264/libx265 may
         * otherwise choose a non-IDR intra picture, which is not sufficient
         * for a client that has discarded its decoder reference state. */
        (void)av_opt_set(encoder->codec_ctx->priv_data, "forced-idr", WD_VIDEO_ENCODER_FORCE_IDR_OPTION, 0);
        if (config->codec == WD_VIDEO_CODEC_H264)
        {
            (void)av_opt_set(encoder->codec_ctx->priv_data, "x264-params", WD_VIDEO_ENCODER_H264_PRIVATE_PARAMS, 0);
        }
        else
        {
            (void)av_opt_set(encoder->codec_ctx->priv_data, "x265-params", WD_VIDEO_ENCODER_H265_PRIVATE_PARAMS, 0);
        }
    }

    const int open_rc = avcodec_open2(encoder->codec_ctx, codec, NULL);
    if (open_rc < 0)
    {
        wd_video_encoder_log_av_error("failed to open software video encoder", open_rc);
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->frame->format = encoder->codec_ctx->pix_fmt;
    encoder->frame->width  = encoder->codec_ctx->width;
    encoder->frame->height = encoder->codec_ctx->height;
    if (av_frame_get_buffer(encoder->frame, WD_FFMPEG_FRAME_ALIGNMENT) < 0)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    const int scaler_flags = WD_VIDEO_SCALER_USE_FAST_BILINEAR ? SWS_FAST_BILINEAR : SWS_BILINEAR;
    encoder->sws_ctx = sws_getContext(encoder->codec_ctx->width, encoder->codec_ctx->height, AV_PIX_FMT_BGRA, encoder->codec_ctx->width,
                                      encoder->codec_ctx->height, encoder->codec_ctx->pix_fmt, scaler_flags, NULL, NULL, NULL);
    if (!encoder->sws_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_SOFTWARE;
    return true;
}


#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
static VADisplay wd_video_encoder_va_display(struct wd_video_encoder* encoder) {
    if (!encoder || !encoder->vaapi_device_ctx)
    {
        return NULL;
    }
    AVHWDeviceContext* hwdev = (AVHWDeviceContext*)encoder->vaapi_device_ctx->data;
    AVVAAPIDeviceContext* va = hwdev ? (AVVAAPIDeviceContext*)hwdev->hwctx : NULL;
    return va ? va->display : NULL;
}

static bool wd_video_encoder_init_vaapi_vpp(struct wd_video_encoder* encoder) {
    VADisplay display = wd_video_encoder_va_display(encoder);
    if (!display || !encoder->codec_ctx)
    {
        return false;
    }

    encoder->vaapi_vpp_config  = VA_INVALID_ID;
    encoder->vaapi_vpp_context = VA_INVALID_ID;
    encoder->vaapi_vpp_ready   = false;

    VAStatus status = vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc,
                                     NULL, 0, &encoder->vaapi_vpp_config);
    if (status != VA_STATUS_SUCCESS)
    {
        return false;
    }

    status = vaCreateContext(display, encoder->vaapi_vpp_config,
                             encoder->codec_ctx->width, encoder->codec_ctx->height,
                             VA_PROGRESSIVE, NULL, 0, &encoder->vaapi_vpp_context);
    if (status != VA_STATUS_SUCCESS)
    {
        (void)vaDestroyConfig(display, encoder->vaapi_vpp_config);
        encoder->vaapi_vpp_config = VA_INVALID_ID;
        return false;
    }

    encoder->vaapi_vpp_ready = true;
    return true;
}

static uint32_t wd_video_encoder_dmabuf_object_size(const struct wd_frame_drm_plane* plane,
                                                     uint32_t height) {
    if (!plane || plane->fd < 0)
    {
        return 0;
    }
    struct stat st;
    if (fstat(plane->fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= UINT32_MAX)
    {
        return (uint32_t)st.st_size;
    }
    const uint64_t fallback = (uint64_t)plane->offset + (uint64_t)plane->stride * height;
    return fallback <= UINT32_MAX ? (uint32_t)fallback : 0;
}

static bool wd_video_encoder_vpp_drm_to_vaapi(struct wd_video_encoder* encoder,
                                               const struct wd_frame* input) {
    if (!encoder || !input || !encoder->vaapi_vpp_ready ||
        input->storage != WD_FRAME_STORAGE_DRM_PRIME ||
        input->data.drm.plane_count != 1 || !encoder->vaapi_frames_ctx ||
        !encoder->frame || input->width != encoder->config.width ||
        input->height != encoder->config.height)
    {
        return false;
    }

    VADisplay display = wd_video_encoder_va_display(encoder);
    if (!display)
    {
        return false;
    }

    const struct wd_frame_drm_plane* plane = &input->data.drm.planes[0];
    const uint32_t object_size = wd_video_encoder_dmabuf_object_size(plane, input->height);
    if (object_size == 0)
    {
        return false;
    }

    VADRMPRIMESurfaceDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fourcc                         = input->fourcc;
    descriptor.width                          = input->width;
    descriptor.height                         = input->height;
    descriptor.num_objects                    = 1;
    descriptor.objects[0].fd                  = plane->fd;
    descriptor.objects[0].size                = object_size;
    descriptor.objects[0].drm_format_modifier = plane->modifier;
    descriptor.num_layers                     = 1;
    descriptor.layers[0].drm_format           = input->fourcc;
    descriptor.layers[0].num_planes           = 1;
    descriptor.layers[0].object_index[0]      = 0;
    descriptor.layers[0].offset[0]            = plane->offset;
    descriptor.layers[0].pitch[0]             = plane->stride;

    VASurfaceAttrib attributes[3];
    memset(attributes, 0, sizeof(attributes));
    attributes[0].type          = VASurfaceAttribMemoryType;
    attributes[0].flags         = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type    = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type          = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].flags         = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type    = VAGenericValueTypePointer;
    attributes[1].value.value.p = &descriptor;
    attributes[2].type          = VASurfaceAttribPixelFormat;
    attributes[2].flags         = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[2].value.type    = VAGenericValueTypeInteger;
    attributes[2].value.value.i = (int)input->fourcc;

    VASurfaceID source_surface = VA_INVALID_SURFACE;
    VAStatus status = vaCreateSurfaces(display, VA_RT_FORMAT_RGB32,
                                       input->width, input->height,
                                       &source_surface, 1, attributes, 3);
    if (status != VA_STATUS_SUCCESS)
    {
        return false;
    }

    av_frame_unref(encoder->frame);
    int rc = av_hwframe_get_buffer(encoder->vaapi_frames_ctx, encoder->frame, 0);
    if (rc < 0)
    {
        (void)vaDestroySurfaces(display, &source_surface, 1);
        return false;
    }

    const VASurfaceID destination_surface = (VASurfaceID)(uintptr_t)encoder->frame->data[3];
    VARectangle source_rect = {0, 0, (uint16_t)input->width, (uint16_t)input->height};
    VARectangle destination_rect = source_rect;
    VAProcPipelineParameterBuffer params;
    memset(&params, 0, sizeof(params));
    params.surface       = source_surface;
    params.surface_region = &source_rect;
    params.output_region  = &destination_rect;

    VABufferID params_buffer = VA_INVALID_ID;
    bool ok = false;
    status = vaCreateBuffer(display, encoder->vaapi_vpp_context,
                            VAProcPipelineParameterBufferType, sizeof(params), 1,
                            &params, &params_buffer);
    if (status == VA_STATUS_SUCCESS &&
        vaBeginPicture(display, encoder->vaapi_vpp_context, destination_surface) == VA_STATUS_SUCCESS)
    {
        status = vaRenderPicture(display, encoder->vaapi_vpp_context, &params_buffer, 1);
        if (status == VA_STATUS_SUCCESS)
        {
            status = vaEndPicture(display, encoder->vaapi_vpp_context);
            if (status == VA_STATUS_SUCCESS)
            {
                /* Synchronize before releasing the imported source descriptor.
                 * Encoding remains GPU-resident; this only establishes source
                 * lifetime across the VPP operation. */
                ok = vaSyncSurface(display, destination_surface) == VA_STATUS_SUCCESS;
            }
        }
        else
        {
            (void)vaEndPicture(display, encoder->vaapi_vpp_context);
        }
    }

    if (params_buffer != VA_INVALID_ID)
    {
        (void)vaDestroyBuffer(display, params_buffer);
    }
    (void)vaDestroySurfaces(display, &source_surface, 1);

    if (!ok)
    {
        av_frame_unref(encoder->frame);
        return false;
    }

    encoder->frame->pts       = (int64_t)input->pts_usec;
    encoder->frame->pict_type = encoder->keyframe_requested ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    return true;
}
#endif

static bool wd_video_encoder_configure_vaapi(struct wd_video_encoder* encoder, const struct wd_video_encoder_config* config,
                                              bool quality_mode) {
    if (!wd_video_encoder_select_vaapi_device_for_config(encoder, config))
    {
        return false;
    }

    const AVCodec* codec = wd_video_encoder_find_vaapi_codec(config->codec);
    if (!codec || !wd_video_encoder_allocate_common(encoder, codec, config, AV_PIX_FMT_VAAPI))
    {
        return false;
    }

    encoder->upload_frame     = av_frame_alloc();
    encoder->vaapi_frames_ctx = av_hwframe_ctx_alloc(encoder->vaapi_device_ctx);
    if (!encoder->upload_frame || !encoder->vaapi_frames_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    AVHWFramesContext* frames = (AVHWFramesContext*)encoder->vaapi_frames_ctx->data;
    frames->format            = AV_PIX_FMT_VAAPI;
    frames->sw_format         = AV_PIX_FMT_NV12;
    frames->width             = encoder->codec_ctx->width;
    frames->height            = encoder->codec_ctx->height;
    frames->initial_pool_size = WD_VIDEO_ENCODER_VAAPI_FRAME_POOL_SIZE;

    int rc = av_hwframe_ctx_init(encoder->vaapi_frames_ctx);
    if (rc < 0)
    {
        wd_video_encoder_log_av_error("failed to initialize VAAPI frame pool", rc);
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->codec_ctx->hw_frames_ctx = av_buffer_ref(encoder->vaapi_frames_ctx);
    if (!encoder->codec_ctx->hw_frames_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    if (encoder->codec_ctx->priv_data)
    {
        (void)av_opt_set(encoder->codec_ctx->priv_data, "async_depth", WD_VIDEO_ENCODER_VAAPI_ASYNC_DEPTH, 0);
        if (quality_mode)
        {
            /* For a generous link, prefer predictable near-lossless HEVC
             * quantization over auto VA-API rate control at ~800 Mbit/s.
             * Do not pair constant QP with a contradictory bit-rate target. */
            encoder->codec_ctx->bit_rate = 0;
            if (av_opt_set(encoder->codec_ctx->priv_data, "rc_mode", "CQP", 0) < 0 ||
                av_opt_set_int(encoder->codec_ctx->priv_data, "qp", WD_VIDEO_HEVC_VAAPI_HIGH_BANDWIDTH_QP, 0) < 0)
            {
                WD_LOG_WARN("HEVC VAAPI quality mode unsupported by FFmpeg; retrying ordinary rate control");
                wd_video_encoder_release_backend(encoder);
                return false;
            }
        }
    }

    rc = avcodec_open2(encoder->codec_ctx, codec, NULL);
    if (rc < 0)
    {
        if (!quality_mode)
        {
            wd_video_encoder_log_av_error("failed to open VAAPI video encoder", rc);
        }
        wd_video_encoder_release_backend(encoder);
        return false;
    }
    if (quality_mode)
    {
        WD_LOG_INFO("HEVC VAAPI quality mode: rc=CQP qp=%d requested_budget_kib_per_sec=%u",
                    WD_VIDEO_HEVC_VAAPI_HIGH_BANDWIDTH_QP, config->bitrate_kib_per_second);
    }

    encoder->upload_frame->format = AV_PIX_FMT_NV12;
    encoder->upload_frame->width  = encoder->codec_ctx->width;
    encoder->upload_frame->height = encoder->codec_ctx->height;
    if (av_frame_get_buffer(encoder->upload_frame, WD_FFMPEG_FRAME_ALIGNMENT) < 0)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    const int scaler_flags = WD_VIDEO_SCALER_USE_FAST_BILINEAR ? SWS_FAST_BILINEAR : SWS_BILINEAR;
    encoder->sws_ctx = sws_getContext(encoder->codec_ctx->width, encoder->codec_ctx->height, AV_PIX_FMT_BGRA, encoder->codec_ctx->width,
                                      encoder->codec_ctx->height, AV_PIX_FMT_NV12, scaler_flags, NULL, NULL, NULL);
    if (!encoder->sws_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP
    if (!wd_video_encoder_init_vaapi_vpp(encoder))
    {
        WD_LOG_DEBUG("VAAPI video processing unavailable; DRM PRIME capture will use CPU readback");
    }
#endif

    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_VAAPI;
    return true;
}

static void wd_video_encoder_release_avpacket(void* user_data, uint8_t* data, size_t size) {
    (void)data;
    (void)size;
    AVPacket* packet = user_data;
    av_packet_free(&packet);
}

static bool wd_video_encoder_own_packet(struct wd_video_encoder* encoder, AVPacket* src, uint64_t fallback_pts_usec,
                                        struct wd_video_encoder_packet* packet) {
    if (!encoder || !src || !packet || src->size <= 0 || (uint64_t)src->size > WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES)
    {
        return false;
    }

    size_t packet_size = (size_t)src->size;
    if (encoder->active_backend == WD_VIDEO_ENCODER_BACKEND_VAAPI && encoder->config.codec == WD_VIDEO_CODEC_H265)
    {
        if (av_packet_make_writable(src) < 0)
        {
            return false;
        }
        const int repaired = wd_hevc_annexb_repair_vaapi(src->data, &packet_size);
        if (repaired < 0)
        {
            WD_LOG_WARN("VAAPI HEVC encoder emitted a packet without a valid Annex-B start code; rejecting frame");
            encoder->keyframe_requested = true;
            return false;
        }
        src->size = (int)packet_size;
    }

    AVPacket* owned_packet = av_packet_clone(src);
    if (!owned_packet)
    {
        return false;
    }
    struct wd_buffer* buffer =
        wd_buffer_wrap(owned_packet->data, packet_size, wd_video_encoder_release_avpacket, owned_packet);
    if (!buffer)
    {
        av_packet_free(&owned_packet);
        return false;
    }

    memset(packet, 0, sizeof(*packet));
    packet->header.session_id       = encoder->config.session_id;
    packet->header.connection_token = encoder->config.connection_token;
    packet->header.content_epoch    = encoder->config.content_epoch;
    packet->header.codec            = encoder->config.codec;
    packet->header.flags            = 0;
    if ((src->flags & AV_PKT_FLAG_KEY) != 0)
    {
        packet->header.flags |= WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_CONFIG;
    }
    packet->header.frame_id     = encoder->next_frame_id++;
    packet->header.pts_usec     = src->pts != AV_NOPTS_VALUE ? (uint64_t)src->pts : fallback_pts_usec;
    packet->header.width        = encoder->config.width;
    packet->header.height       = encoder->config.height;
    packet->header.coded_width  = (uint16_t)encoder->codec_ctx->width;
    packet->header.coded_height = (uint16_t)encoder->codec_ctx->height;
    packet->header.data_size    = (uint32_t)packet_size;
    packet->buffer              = buffer;
    packet->data                = wd_buffer_const_data(buffer);
    return true;
}

#endif

static bool wd_video_encoder_parse_preference(const char* backend, enum wd_video_encoder_preference* preference) {
    if (!preference)
    {
        return false;
    }

    if (!backend || backend[0] == '\0' || strcmp(backend, "auto") == 0)
    {
        *preference = WD_VIDEO_ENCODER_PREFERENCE_AUTO;
        return true;
    }
    if (strcmp(backend, "off") == 0)
    {
        *preference = WD_VIDEO_ENCODER_PREFERENCE_OFF;
        return true;
    }
    if (strcmp(backend, "software") == 0)
    {
        *preference = WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE;
        return true;
    }
    if (strcmp(backend, "vaapi") == 0)
    {
        *preference = WD_VIDEO_ENCODER_PREFERENCE_VAAPI;
        return true;
    }
    return false;
}

bool wd_video_encoder_create(struct wd_video_encoder** out_encoder, const char* video_encoder_backend) {
    if (!out_encoder)
    {
        return false;
    }

    *out_encoder = NULL;
    enum wd_video_encoder_preference preference;
    if (!wd_video_encoder_parse_preference(video_encoder_backend, &preference))
    {
        return false;
    }

    struct wd_video_encoder* encoder = calloc(1, sizeof(*encoder));
    if (!encoder)
    {
        return false;
    }
    encoder->preference     = preference;
    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_NONE;

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    av_log_set_level(wd_log_is_verbose() ? AV_LOG_WARNING : AV_LOG_ERROR);
#endif

    *out_encoder = encoder;
    return true;
}

void wd_video_encoder_destroy(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return;
    }
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    wd_video_encoder_release_backend(encoder);
    av_buffer_unref(&encoder->vaapi_device_ctx);
#endif
    free(encoder);
}

void wd_video_encoder_reset(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    wd_video_encoder_release_backend(encoder);
#endif

    memset(&encoder->config, 0, sizeof(encoder->config));
    encoder->configured         = false;
    encoder->keyframe_requested = false;
    encoder->next_frame_id      = 0;
    encoder->vaapi_failed_codecs = 0;
}

bool wd_video_encoder_available(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    return encoder && encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_OFF &&
           wd_video_encoder_detect_supported_codecs((struct wd_video_encoder*)encoder) != 0;
#else
    (void)encoder;
    return false;
#endif
}

uint32_t wd_video_encoder_supported_codecs(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    return encoder && encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_OFF
               ? wd_video_encoder_detect_supported_codecs((struct wd_video_encoder*)encoder)
               : 0;
#else
    (void)encoder;
    return 0;
#endif
}

uint32_t wd_video_encoder_choose_codec(struct wd_video_encoder* encoder, uint32_t client_codecs) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    if (!encoder || encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_OFF)
    {
        return 0;
    }

    const uint32_t requested = client_codecs & WD_VIDEO_CODEC_MASK;
    if (requested == 0)
    {
        return 0;
    }

    /* A software-only configuration must never initialize/probe a VA-API device.
     * Similarly, a forced VA-API configuration need not discover software codecs. */
    const uint32_t software = encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_VAAPI
                                  ? requested & wd_video_encoder_detect_software_codecs()
                                  : 0;
    const uint32_t vaapi = encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE
                               ? requested & wd_video_encoder_detect_vaapi_codecs(encoder) & ~encoder->vaapi_failed_codecs
                               : 0;

    if (encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE)
    {
        if ((vaapi & WD_VIDEO_CODEC_H265) != 0)
        {
            return WD_VIDEO_CODEC_H265;
        }
        if ((vaapi & WD_VIDEO_CODEC_H264) != 0)
        {
            return WD_VIDEO_CODEC_H264;
        }
        if ((vaapi & WD_VIDEO_CODEC_AV1) != 0)
        {
            return WD_VIDEO_CODEC_AV1;
        }
    }

    if (encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_VAAPI)
    {
        if ((software & WD_VIDEO_CODEC_H265) != 0)
        {
            return WD_VIDEO_CODEC_H265;
        }
        if ((software & WD_VIDEO_CODEC_H264) != 0)
        {
            return WD_VIDEO_CODEC_H264;
        }
        if ((software & WD_VIDEO_CODEC_AV1) != 0)
        {
            return WD_VIDEO_CODEC_AV1;
        }
    }
#else
    (void)encoder;
    (void)client_codecs;
#endif
    return 0;
}

const char* wd_video_encoder_backend_name(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    if (encoder && encoder->codec && encoder->codec->name)
    {
        return encoder->codec->name;
    }
#endif
    if (!encoder)
    {
        return "none";
    }
    switch (encoder->preference)
    {
    case WD_VIDEO_ENCODER_PREFERENCE_OFF:
        return "off";
    case WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE:
        return "software";
    case WD_VIDEO_ENCODER_PREFERENCE_VAAPI:
        return "vaapi";
    case WD_VIDEO_ENCODER_PREFERENCE_AUTO:
    default:
        return "auto";
    }
}

bool wd_video_encoder_configure(struct wd_video_encoder* encoder, const struct wd_video_encoder_config* config) {
    if (!encoder || encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_OFF || !config ||
        (config->codec != WD_VIDEO_CODEC_H265 && config->codec != WD_VIDEO_CODEC_H264 && config->codec != WD_VIDEO_CODEC_AV1) || config->width == 0 ||
        config->height == 0)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    if (wd_video_encoder_config_matches(encoder, config))
    {
        return true;
    }

    const bool new_session = !encoder->configured || encoder->config.session_id != config->session_id ||
                             encoder->config.connection_token != config->connection_token ||
                             encoder->config.content_epoch != config->content_epoch;
    if (new_session)
    {
        encoder->vaapi_failed_codecs = 0;
    }

    wd_video_encoder_release_backend(encoder);
    encoder->configured = false;

    bool configured      = false;
    bool vaapi_attempted = false;
    if (encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE &&
        (encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_VAAPI || (encoder->vaapi_failed_codecs & config->codec) == 0))
    {
        vaapi_attempted = true;
        const bool prefer_quality = wd_video_hevc_vaapi_prefer_quality(config->codec, config->bitrate_kib_per_second);
        configured = wd_video_encoder_configure_vaapi(encoder, config, prefer_quality);
        if (!configured && prefer_quality)
        {
            WD_LOG_WARN("HEVC VAAPI quality mode unavailable; retrying ordinary VAAPI rate control");
            configured = wd_video_encoder_configure_vaapi(encoder, config, false);
        }
        if (configured)
        {
            encoder->vaapi_failed_codecs &= ~config->codec;
        }
        else
        {
            encoder->vaapi_failed_codecs |= config->codec;
            if (encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_AUTO)
            {
                WD_LOG_WARN("VAAPI %s encoder unavailable on %s; falling back to software", wd_video_encoder_codec_name(config->codec),
                            encoder->vaapi_device);
            }
        }
    }

    if (!configured && encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_VAAPI)
    {
        configured = wd_video_encoder_configure_software(encoder, config);
    }

    if (!configured)
    {
        if (vaapi_attempted && encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_VAAPI)
        {
            WD_LOG_WARN("forced VAAPI %s encoder unavailable on %s", wd_video_encoder_codec_name(config->codec), encoder->vaapi_device);
        }
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->config             = *config;
    encoder->configured         = true;
    encoder->keyframe_requested = true;
    if (new_session || encoder->next_frame_id == 0)
    {
        encoder->next_frame_id = 1;
    }

    WD_LOG_DEBUG("video encoder configured: backend=%s codec=%s size=%ux%u fps=%u bitrate_kib=%u%s%s",
                 encoder->codec && encoder->codec->name ? encoder->codec->name : "unknown", wd_video_encoder_codec_name(config->codec),
                 config->width, config->height, config->target_fps, config->bitrate_kib_per_second,
                 encoder->active_backend == WD_VIDEO_ENCODER_BACKEND_VAAPI ? " device=" : "",
                 encoder->active_backend == WD_VIDEO_ENCODER_BACKEND_VAAPI ? encoder->vaapi_device : "");
    return true;
#else
    encoder->config             = *config;
    encoder->configured         = true;
    encoder->keyframe_requested = true;
    encoder->next_frame_id      = 1;
    return wd_video_encoder_available(encoder);
#endif
}

bool wd_video_encoder_adopt_content_epoch(struct wd_video_encoder* encoder, uint8_t session_id,
                                          uint64_t connection_token, uint64_t old_epoch, uint64_t new_epoch) {
    if (!encoder || !encoder->configured || old_epoch == 0 || new_epoch == 0 || old_epoch == new_epoch ||
        encoder->config.session_id != session_id || encoder->config.connection_token != connection_token ||
        encoder->config.content_epoch != old_epoch)
    {
        return false;
    }

    /* The first keyframe is encoded under the tile epoch, but transmitted
     * under the new video epoch. Update only the encoder's ownership marker:
     * resetting its codec context here would produce another frame-1 IDR
     * and force an unnecessary client decoder restart. */
    encoder->config.content_epoch = new_epoch;
    return true;
}

bool wd_video_encoder_request_keyframe(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return false;
    }

    encoder->keyframe_requested = true;
    return wd_video_encoder_available(encoder);
}

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
static const uint32_t* wd_video_encoder_prepare_xrgb_source(struct wd_video_encoder*                      encoder,
                                                            const struct wd_video_encoder_input_xrgb8888* input, uint32_t coded_width,
                                                            uint32_t coded_height, uint32_t* out_stride_pixels) {
    if (!encoder || !input || !out_stride_pixels || coded_width == 0 || coded_height == 0)
    {
        return NULL;
    }

    if (input->width == coded_width && input->height == coded_height)
    {
        *out_stride_pixels = input->stride_pixels;
        return input->pixels;
    }

    const size_t needed = (size_t)coded_width * (size_t)coded_height;
    if (encoder->padded_pixel_capacity < needed)
    {
        uint32_t* new_pixels = realloc(encoder->padded_pixels, needed * sizeof(*new_pixels));
        if (!new_pixels)
        {
            return NULL;
        }
        encoder->padded_pixels         = new_pixels;
        encoder->padded_pixel_capacity = needed;
    }

    for (uint32_t y = 0; y < coded_height; ++y)
    {
        const uint32_t  src_y = y < input->height ? y : input->height - 1u;
        const uint32_t* src   = input->pixels + (size_t)src_y * input->stride_pixels;
        uint32_t*       dst   = encoder->padded_pixels + (size_t)y * coded_width;
        memcpy(dst, src, (size_t)input->width * sizeof(*dst));
        const uint32_t edge = input->width != 0 ? src[input->width - 1u] : 0xff000000u;
        for (uint32_t x = input->width; x < coded_width; ++x)
        {
            dst[x] = edge;
        }
    }

    *out_stride_pixels = coded_width;
    return encoder->padded_pixels;
}

static bool wd_video_encoder_prepare_frame(struct wd_video_encoder* encoder, const struct wd_video_encoder_input_xrgb8888* input) {
    uint32_t        source_stride_pixels = 0;
    const uint32_t* source_pixels = wd_video_encoder_prepare_xrgb_source(encoder, input, (uint32_t)encoder->codec_ctx->width,
                                                                         (uint32_t)encoder->codec_ctx->height, &source_stride_pixels);
    if (!source_pixels)
    {
        return false;
    }

    const uint8_t* src_slices[4] = {(const uint8_t*)source_pixels, NULL, NULL, NULL};
    const int      src_stride[4] = {(int)(source_stride_pixels * WD_BYTES_PER_PIXEL), 0, 0, 0};

    if (encoder->active_backend == WD_VIDEO_ENCODER_BACKEND_VAAPI)
    {
        if (!encoder->upload_frame || !encoder->vaapi_frames_ctx || av_frame_make_writable(encoder->upload_frame) < 0)
        {
            return false;
        }

        if (sws_scale(encoder->sws_ctx, src_slices, src_stride, 0, encoder->codec_ctx->height, encoder->upload_frame->data,
                      encoder->upload_frame->linesize) != encoder->upload_frame->height)
        {
            return false;
        }

        av_frame_unref(encoder->frame);
        int rc = av_hwframe_get_buffer(encoder->vaapi_frames_ctx, encoder->frame, 0);
        if (rc < 0)
        {
            wd_video_encoder_log_av_error("failed to acquire VAAPI encode surface", rc);
            return false;
        }
        rc = av_hwframe_transfer_data(encoder->frame, encoder->upload_frame, 0);
        if (rc < 0)
        {
            wd_video_encoder_log_av_error("failed to upload frame to VAAPI encode surface", rc);
            return false;
        }
    }
    else
    {
        if (av_frame_make_writable(encoder->frame) < 0)
        {
            return false;
        }
        if (sws_scale(encoder->sws_ctx, src_slices, src_stride, 0, encoder->codec_ctx->height, encoder->frame->data,
                      encoder->frame->linesize) != encoder->frame->height)
        {
            return false;
        }
    }

    encoder->frame->pts       = (int64_t)input->pts_usec;
    encoder->frame->pict_type = encoder->keyframe_requested ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    return true;
}
#endif

void wd_video_encoder_packet_release(struct wd_video_encoder_packet* packet) {
    if (!packet)
    {
        return;
    }
    wd_buffer_release(packet->buffer);
    memset(packet, 0, sizeof(*packet));
}


#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
static bool wd_video_encoder_encode_prepared(struct wd_video_encoder* encoder, uint64_t pts_usec,
                                             struct wd_video_encoder_packet* packet) {
    bool frame_sent  = false;
    bool have_output = false;

    for (int attempt = 0; attempt < 2 && !frame_sent; ++attempt)
    {
        int rc = avcodec_send_frame(encoder->codec_ctx, encoder->frame);
        if (rc == 0)
        {
            frame_sent = true;
            break;
        }
        if (rc != AVERROR(EAGAIN))
        {
            wd_video_encoder_log_av_error("failed to submit video frame to encoder", rc);
            return false;
        }

        for (;;)
        {
            av_packet_unref(encoder->packet);
            rc = avcodec_receive_packet(encoder->codec_ctx, encoder->packet);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
            {
                break;
            }
            if (rc < 0)
            {
                wd_video_encoder_log_av_error("failed to receive encoded video packet", rc);
                encoder->keyframe_requested = true;
                return false;
            }
            if (!have_output)
            {
                if (!wd_video_encoder_own_packet(encoder, encoder->packet, pts_usec, packet))
                {
                    av_packet_unref(encoder->packet);
                    encoder->keyframe_requested = true;
                    return false;
                }
                have_output = true;
            }
        }
    }

    if (!frame_sent)
    {
        encoder->keyframe_requested = true;
        return false;
    }

    for (;;)
    {
        av_packet_unref(encoder->packet);
        int rc = avcodec_receive_packet(encoder->codec_ctx, encoder->packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
        {
            break;
        }
        if (rc < 0)
        {
            wd_video_encoder_log_av_error("failed to receive encoded video packet", rc);
            encoder->keyframe_requested = true;
            return false;
        }
        if (!have_output)
        {
            if (!wd_video_encoder_own_packet(encoder, encoder->packet, pts_usec, packet))
            {
                av_packet_unref(encoder->packet);
                encoder->keyframe_requested = true;
                return false;
            }
            have_output = true;
        }
    }

    encoder->keyframe_requested = false;
    return true;
}
#endif

bool wd_video_encoder_encode_xrgb8888(struct wd_video_encoder* encoder, const struct wd_video_encoder_input_xrgb8888* input,
                                      struct wd_video_encoder_packet* packet) {
    if (packet)
    {
        memset(packet, 0, sizeof(*packet));
    }

    if (!encoder || !input || !packet || !input->pixels || input->width == 0 || input->height == 0 || input->stride_pixels < input->width)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER
    if (!encoder->configured || !encoder->codec_ctx || !encoder->frame || !encoder->packet || !encoder->sws_ctx ||
        input->width != encoder->config.width || input->height != encoder->config.height)
    {
        return false;
    }

    if (!wd_video_encoder_prepare_frame(encoder, input))
    {
        return false;
    }

    return wd_video_encoder_encode_prepared(encoder, input->pts_usec, packet);
#else
    (void)encoder;
    (void)input;
    return false;
#endif
}


bool wd_video_encoder_supports_drm_prime(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP && (WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER)
    return encoder && encoder->configured &&
           encoder->active_backend == WD_VIDEO_ENCODER_BACKEND_VAAPI &&
           encoder->vaapi_vpp_ready;
#else
    (void)encoder;
    return false;
#endif
}

bool wd_video_encoder_encode_frame(struct wd_video_encoder* encoder, const struct wd_frame* frame,
                                   struct wd_video_encoder_packet* packet) {
    if (!encoder || !frame || !packet || !wd_frame_valid(frame))
    {
        return false;
    }

    if (frame->storage == WD_FRAME_STORAGE_CPU_XRGB8888)
    {
        if ((frame->data.cpu.stride_bytes & 3u) != 0)
        {
            return false;
        }
        const uint8_t* bytes = wd_frame_cpu_data(frame);
        if (!bytes)
        {
            return false;
        }
        struct wd_video_encoder_input_xrgb8888 input = {
            .pixels        = (const uint32_t*)bytes,
            .width         = frame->width,
            .height        = frame->height,
            .stride_pixels = frame->data.cpu.stride_bytes / 4u,
            .pts_usec      = frame->pts_usec,
        };
        return wd_video_encoder_encode_xrgb8888(encoder, &input, packet);
    }

#if WAYDISPLAY_HAVE_VAAPI_SERVER_VPP && (WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER || WAYDISPLAY_HAVE_AV1_SERVER_ENCODER)
    if (frame->storage == WD_FRAME_STORAGE_DRM_PRIME)
    {
        memset(packet, 0, sizeof(*packet));
        if (!wd_video_encoder_supports_drm_prime(encoder) ||
            frame->width != encoder->config.width || frame->height != encoder->config.height ||
            !wd_video_encoder_vpp_drm_to_vaapi(encoder, frame))
        {
            return false;
        }
        return wd_video_encoder_encode_prepared(encoder, frame->pts_usec, packet);
    }
#endif

    return false;
}
