#include "wd_video_encoder.h"

#include "waydisplay/wd_log.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WAYDISPLAY_HAVE_H265_SERVER_ENCODER
#define WAYDISPLAY_HAVE_H265_SERVER_ENCODER 0
#endif

#ifndef WAYDISPLAY_HAVE_H264_SERVER_ENCODER
#define WAYDISPLAY_HAVE_H264_SERVER_ENCODER 0
#endif

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
enum {
    /*
     * Use a codec-block-aligned probe frame large enough for drivers whose
     * minimum encode width is greater than 128 pixels (for example 130).
     * This is a one-time capability probe, so the extra surface size is
     * negligible and avoids false negatives from an undersized test frame.
     */
    WD_VAAPI_PROBE_WIDTH = 256,
    WD_VAAPI_PROBE_HEIGHT = 256,
};
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#endif

enum wd_video_encoder_preference {
    WD_VIDEO_ENCODER_PREFERENCE_AUTO = 0,
    WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE,
    WD_VIDEO_ENCODER_PREFERENCE_VAAPI,
};

enum wd_video_encoder_backend {
    WD_VIDEO_ENCODER_BACKEND_NONE = 0,
    WD_VIDEO_ENCODER_BACKEND_SOFTWARE,
    WD_VIDEO_ENCODER_BACKEND_VAAPI,
};

static char* wd_video_encoder_strdup(const char* text) {
    if (!text)
    {
        return NULL;
    }

    const size_t length = strlen(text) + 1;
    char* copy = malloc(length);
    if (copy)
    {
        memcpy(copy, text, length);
    }
    return copy;
}

struct wd_video_encoder {
    struct wd_video_encoder_config config;
    bool                           configured;
    bool                           keyframe_requested;
    uint64_t                       next_frame_id;
    enum wd_video_encoder_preference preference;
    enum wd_video_encoder_backend    active_backend;
    char*                            vaapi_device;
    uint32_t                         vaapi_failed_codecs;

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    const AVCodec*     codec;
    AVCodecContext*    codec_ctx;
    AVFrame*           frame;
    AVFrame*           upload_frame;
    AVPacket*          packet;
    struct SwsContext* sws_ctx;
    AVBufferRef*       vaapi_device_ctx;
    AVBufferRef*       vaapi_frames_ctx;
    bool               vaapi_device_attempted;
    bool               vaapi_probe_complete;
    uint32_t           vaapi_supported_codecs;
    uint32_t*          padded_pixels;
    size_t             padded_pixel_capacity;
    uint8_t*           packet_copy;
    size_t             packet_copy_capacity;
#endif
};

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
static const char* wd_video_encoder_codec_name(uint32_t codec) {
    switch (codec)
    {
    case WD_VIDEO_CODEC_H264:
        return "h264";
    case WD_VIDEO_CODEC_H265:
        return "h265";
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

    sws_freeContext(encoder->sws_ctx);
    encoder->sws_ctx = NULL;

    free(encoder->padded_pixels);
    encoder->padded_pixels = NULL;
    encoder->padded_pixel_capacity = 0;

    free(encoder->packet_copy);
    encoder->packet_copy = NULL;
    encoder->packet_copy_capacity = 0;

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
    {
        const AVCodec* avc = avcodec_find_encoder_by_name("libx264");
        return avc ? avc : avcodec_find_encoder(AV_CODEC_ID_H264);
    }
#endif
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    case WD_VIDEO_CODEC_H265:
    {
        const AVCodec* hevc = avcodec_find_encoder_by_name("libx265");
        return hevc ? hevc : avcodec_find_encoder(AV_CODEC_ID_HEVC);
    }
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
    default:
        return NULL;
    }
}

static bool wd_video_encoder_probe_vaapi_codec(struct wd_video_encoder* encoder, uint32_t codec);

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
    return codecs;
}

static bool wd_video_encoder_ensure_vaapi_device(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return false;
    }
    if (encoder->vaapi_device_ctx)
    {
        return true;
    }
    if (encoder->vaapi_device_attempted)
    {
        return false;
    }

    encoder->vaapi_device_attempted = true;
    const char* device = encoder->vaapi_device && encoder->vaapi_device[0] != '\0'
                             ? encoder->vaapi_device
                             : "/dev/dri/renderD128";
    const int rc = av_hwdevice_ctx_create(&encoder->vaapi_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                          device, NULL, 0);
    if (rc < 0)
    {
        wd_video_encoder_log_av_error("failed to create VAAPI encode device", rc);
        av_buffer_unref(&encoder->vaapi_device_ctx);
        return false;
    }

    WD_LOG_INFO("VAAPI video encode device initialized: %s", device);
    return true;
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

    encoder->vaapi_probe_complete = true;
    encoder->vaapi_supported_codecs = 0;
    if (!wd_video_encoder_ensure_vaapi_device(encoder))
    {
        return 0;
    }

#if WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    if (wd_video_encoder_find_vaapi_codec(WD_VIDEO_CODEC_H264) &&
        wd_video_encoder_probe_vaapi_codec(encoder, WD_VIDEO_CODEC_H264))
    {
        encoder->vaapi_supported_codecs |= WD_VIDEO_CODEC_H264;
    }
#endif
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (wd_video_encoder_find_vaapi_codec(WD_VIDEO_CODEC_H265) &&
        wd_video_encoder_probe_vaapi_codec(encoder, WD_VIDEO_CODEC_H265))
    {
        encoder->vaapi_supported_codecs |= WD_VIDEO_CODEC_H265;
    }
#endif

    WD_LOG_INFO("VAAPI video encode codecs on %s: h264=%s h265=%s",
                encoder->vaapi_device,
                (encoder->vaapi_supported_codecs & WD_VIDEO_CODEC_H264) != 0 ? "yes" : "no",
                (encoder->vaapi_supported_codecs & WD_VIDEO_CODEC_H265) != 0 ? "yes" : "no");
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
               wd_video_encoder_detect_vaapi_codecs(encoder);
    }
}

static bool wd_video_encoder_config_matches(const struct wd_video_encoder* encoder,
                                            const struct wd_video_encoder_config* config) {
    return encoder && config && encoder->configured && encoder->codec_ctx &&
           encoder->config.session_id == config->session_id && encoder->config.connection_token == config->connection_token && encoder->config.content_epoch == config->content_epoch && encoder->config.width == config->width &&
           encoder->config.height == config->height && encoder->config.target_fps == config->target_fps &&
           encoder->config.bitrate_kib_per_second == config->bitrate_kib_per_second &&
           encoder->config.codec == config->codec;
}

static int wd_video_encoder_effective_fps(const struct wd_video_encoder_config* config) {
    if (!config || config->target_fps == 0)
    {
        return 30;
    }

    return config->target_fps;
}

static int64_t wd_video_encoder_bitrate_bits_per_second(const struct wd_video_encoder_config* config) {
    if (!config || config->bitrate_kib_per_second == 0)
    {
        return 8ll * 1024ll * 1024ll;
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

static void wd_video_encoder_set_context_defaults(AVCodecContext* codec_ctx,
                                                  const AVCodec* codec,
                                                  const struct wd_video_encoder_config* config,
                                                  enum AVPixelFormat pixel_format) {
    const int fps = wd_video_encoder_effective_fps(config);
    const int64_t bitrate = wd_video_encoder_bitrate_bits_per_second(config);

    codec_ctx->codec_type   = AVMEDIA_TYPE_VIDEO;
    codec_ctx->codec_id     = codec ? codec->id : AV_CODEC_ID_NONE;
    codec_ctx->width        = wd_video_encoder_even_dimension(config->width);
    codec_ctx->height       = wd_video_encoder_even_dimension(config->height);
    codec_ctx->pix_fmt      = pixel_format;
    codec_ctx->time_base    = (AVRational){1, 1000000};
    codec_ctx->framerate    = (AVRational){fps, 1};
    codec_ctx->gop_size     = fps > 0 ? fps : 30;
    codec_ctx->max_b_frames = 0;
    codec_ctx->bit_rate     = bitrate;
}

static bool wd_video_encoder_probe_vaapi_codec(struct wd_video_encoder* encoder, uint32_t codec_id) {
    if (!encoder || !encoder->vaapi_device_ctx)
    {
        return false;
    }

    const AVCodec* codec = wd_video_encoder_find_vaapi_codec(codec_id);
    if (!codec)
    {
        return false;
    }

    struct wd_video_encoder_config probe_config;
    memset(&probe_config, 0, sizeof(probe_config));
    probe_config.width = WD_VAAPI_PROBE_WIDTH;
    probe_config.height = WD_VAAPI_PROBE_HEIGHT;
    probe_config.target_fps = 30;
    probe_config.bitrate_kib_per_second = 2048;
    probe_config.codec = codec_id;

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    AVBufferRef* frames_ref = NULL;
    bool supported = false;
    if (!codec_ctx)
    {
        return false;
    }

    wd_video_encoder_set_context_defaults(codec_ctx, codec, &probe_config, AV_PIX_FMT_VAAPI);
    frames_ref = av_hwframe_ctx_alloc(encoder->vaapi_device_ctx);
    if (!frames_ref)
    {
        goto done;
    }

    AVHWFramesContext* frames = (AVHWFramesContext*)frames_ref->data;
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = codec_ctx->width;
    frames->height = codec_ctx->height;
    frames->initial_pool_size = 2;
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
        (void)av_opt_set(codec_ctx->priv_data, "async_depth", "1", 0);
    }

    supported = avcodec_open2(codec_ctx, codec, NULL) >= 0;

done:
    avcodec_free_context(&codec_ctx);
    av_buffer_unref(&frames_ref);
    return supported;
}

static bool wd_video_encoder_allocate_common(struct wd_video_encoder* encoder,
                                             const AVCodec* codec,
                                             const struct wd_video_encoder_config* config,
                                             enum AVPixelFormat pixel_format) {
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

static bool wd_video_encoder_configure_software(struct wd_video_encoder* encoder,
                                                const struct wd_video_encoder_config* config) {
    const AVCodec* codec = wd_video_encoder_find_software_codec(config->codec);
    if (!codec || !wd_video_encoder_allocate_common(encoder, codec, config, AV_PIX_FMT_YUV420P))
    {
        return false;
    }

    encoder->codec_ctx->thread_count = 2;
    if (encoder->codec_ctx->priv_data)
    {
        (void)av_opt_set(encoder->codec_ctx->priv_data, "preset", "ultrafast", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "tune", "zerolatency", 0);
        if (config->codec == WD_VIDEO_CODEC_H264)
        {
            (void)av_opt_set(encoder->codec_ctx->priv_data, "x264-params",
                             "repeat-headers=1:sliced-threads=1", 0);
        }
        else
        {
            (void)av_opt_set(encoder->codec_ctx->priv_data, "x265-params",
                             "repeat-headers=1:log-level=error:pools=none:frame-threads=1", 0);
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
    if (av_frame_get_buffer(encoder->frame, 32) < 0)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->sws_ctx = sws_getContext(encoder->codec_ctx->width, encoder->codec_ctx->height,
                                      AV_PIX_FMT_BGRA,
                                      encoder->codec_ctx->width, encoder->codec_ctx->height,
                                      encoder->codec_ctx->pix_fmt, SWS_FAST_BILINEAR,
                                      NULL, NULL, NULL);
    if (!encoder->sws_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_SOFTWARE;
    return true;
}

static bool wd_video_encoder_configure_vaapi(struct wd_video_encoder* encoder,
                                             const struct wd_video_encoder_config* config) {
    if (!wd_video_encoder_ensure_vaapi_device(encoder) ||
        (wd_video_encoder_detect_vaapi_codecs(encoder) & config->codec) == 0)
    {
        return false;
    }

    const AVCodec* codec = wd_video_encoder_find_vaapi_codec(config->codec);
    if (!codec || !wd_video_encoder_allocate_common(encoder, codec, config, AV_PIX_FMT_VAAPI))
    {
        return false;
    }

    encoder->upload_frame = av_frame_alloc();
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
    frames->initial_pool_size = 4;

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
        (void)av_opt_set(encoder->codec_ctx->priv_data, "async_depth", "1", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "aud", "1", 0);
    }

    rc = avcodec_open2(encoder->codec_ctx, codec, NULL);
    if (rc < 0)
    {
        wd_video_encoder_log_av_error("failed to open VAAPI video encoder", rc);
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->upload_frame->format = AV_PIX_FMT_NV12;
    encoder->upload_frame->width  = encoder->codec_ctx->width;
    encoder->upload_frame->height = encoder->codec_ctx->height;
    if (av_frame_get_buffer(encoder->upload_frame, 32) < 0)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->sws_ctx = sws_getContext(encoder->codec_ctx->width, encoder->codec_ctx->height,
                                      AV_PIX_FMT_BGRA,
                                      encoder->codec_ctx->width, encoder->codec_ctx->height,
                                      AV_PIX_FMT_NV12, SWS_FAST_BILINEAR,
                                      NULL, NULL, NULL);
    if (!encoder->sws_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_VAAPI;
    return true;
}

static bool wd_video_encoder_copy_packet(struct wd_video_encoder* encoder, const AVPacket* src,
                                         uint64_t fallback_pts_usec,
                                         struct wd_video_encoder_packet* packet) {
    if (!encoder || !src || !packet || src->size <= 0 ||
        (uint64_t)src->size > WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES)
    {
        return false;
    }

    if (encoder->packet_copy_capacity < (size_t)src->size)
    {
        uint8_t* new_copy = realloc(encoder->packet_copy, (size_t)src->size);
        if (!new_copy)
        {
            return false;
        }
        encoder->packet_copy = new_copy;
        encoder->packet_copy_capacity = (size_t)src->size;
    }
    memcpy(encoder->packet_copy, src->data, (size_t)src->size);

    memset(packet, 0, sizeof(*packet));
    packet->header.session_id = encoder->config.session_id;
    packet->header.connection_token = encoder->config.connection_token;
    packet->header.content_epoch = encoder->config.content_epoch;
    packet->header.codec      = encoder->config.codec;
    packet->header.flags      = 0;
    if ((src->flags & AV_PKT_FLAG_KEY) != 0)
    {
        packet->header.flags |= WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_CONFIG;
    }
    packet->header.frame_id      = encoder->next_frame_id++;
    packet->header.pts_usec      = src->pts != AV_NOPTS_VALUE ? (uint64_t)src->pts : fallback_pts_usec;
    packet->header.width         = encoder->config.width;
    packet->header.height        = encoder->config.height;
    packet->header.coded_width   = (uint16_t)encoder->codec_ctx->width;
    packet->header.coded_height  = (uint16_t)encoder->codec_ctx->height;
    packet->header.data_size     = (uint32_t)src->size;
    packet->data                 = encoder->packet_copy;
    return true;
}
#endif

static bool wd_video_encoder_parse_preference(const char* backend,
                                              enum wd_video_encoder_preference* preference) {
    if (!preference)
    {
        return false;
    }

    if (!backend || backend[0] == '\0' || strcmp(backend, "auto") == 0)
    {
        *preference = WD_VIDEO_ENCODER_PREFERENCE_AUTO;
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

bool wd_video_encoder_create(struct wd_video_encoder** out_encoder,
                             const char* video_encoder_backend,
                             const char* vaapi_device) {
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
    encoder->preference = preference;
    encoder->active_backend = WD_VIDEO_ENCODER_BACKEND_NONE;
    encoder->vaapi_device = wd_video_encoder_strdup(vaapi_device && vaapi_device[0] != '\0'
                                       ? vaapi_device
                                       : "/dev/dri/renderD128");
    if (!encoder->vaapi_device)
    {
        free(encoder);
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    av_log_set_level(AV_LOG_WARNING);
    if (preference != WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE)
    {
        (void)wd_video_encoder_ensure_vaapi_device(encoder);
    }
#endif

    *out_encoder = encoder;
    return true;
}

void wd_video_encoder_destroy(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return;
    }
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    wd_video_encoder_release_backend(encoder);
    av_buffer_unref(&encoder->vaapi_device_ctx);
#endif
    free(encoder->vaapi_device);
    free(encoder);
}

void wd_video_encoder_reset(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    wd_video_encoder_release_backend(encoder);
#endif

    memset(&encoder->config, 0, sizeof(encoder->config));
    encoder->configured         = false;
    encoder->keyframe_requested = false;
    encoder->next_frame_id      = 0;
}

bool wd_video_encoder_available(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    return encoder && wd_video_encoder_detect_supported_codecs((struct wd_video_encoder*)encoder) != 0;
#else
    (void)encoder;
    return false;
#endif
}

uint32_t wd_video_encoder_supported_codecs(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    return encoder ? wd_video_encoder_detect_supported_codecs((struct wd_video_encoder*)encoder) : 0;
#else
    (void)encoder;
    return 0;
#endif
}

uint32_t wd_video_encoder_choose_codec(struct wd_video_encoder* encoder, uint32_t client_codecs) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    if (!encoder)
    {
        return 0;
    }

    const uint32_t requested = client_codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265);
    const uint32_t software = requested & wd_video_encoder_detect_software_codecs();
    const uint32_t vaapi = requested & wd_video_encoder_detect_vaapi_codecs(encoder) &
                           ~encoder->vaapi_failed_codecs;

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
    }
#else
    (void)encoder;
    (void)client_codecs;
#endif
    return 0;
}

const char* wd_video_encoder_backend_name(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
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
    case WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE:
        return "software";
    case WD_VIDEO_ENCODER_PREFERENCE_VAAPI:
        return "vaapi";
    case WD_VIDEO_ENCODER_PREFERENCE_AUTO:
    default:
        return "auto";
    }
}

bool wd_video_encoder_configure(struct wd_video_encoder* encoder,
                                const struct wd_video_encoder_config* config) {
    if (!encoder || !config ||
        (config->codec != WD_VIDEO_CODEC_H265 && config->codec != WD_VIDEO_CODEC_H264) ||
        config->width == 0 || config->height == 0)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    if (wd_video_encoder_config_matches(encoder, config))
    {
        return true;
    }

    const bool new_session = !encoder->configured ||
                             encoder->config.session_id != config->session_id ||
                             encoder->config.connection_token != config->connection_token ||
                             encoder->config.content_epoch != config->content_epoch;

    wd_video_encoder_release_backend(encoder);
    encoder->configured = false;

    bool configured = false;
    bool vaapi_attempted = false;
    if (encoder->preference != WD_VIDEO_ENCODER_PREFERENCE_SOFTWARE &&
        (encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_VAAPI ||
         (encoder->vaapi_failed_codecs & config->codec) == 0))
    {
        vaapi_attempted = true;
        configured = wd_video_encoder_configure_vaapi(encoder, config);
        if (configured)
        {
            encoder->vaapi_failed_codecs &= ~config->codec;
        }
        else
        {
            encoder->vaapi_failed_codecs |= config->codec;
            if (encoder->preference == WD_VIDEO_ENCODER_PREFERENCE_AUTO)
            {
                WD_LOG_WARN("VAAPI %s encoder unavailable on %s; falling back to software",
                            wd_video_encoder_codec_name(config->codec), encoder->vaapi_device);
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
            WD_LOG_WARN("forced VAAPI %s encoder unavailable on %s",
                        wd_video_encoder_codec_name(config->codec), encoder->vaapi_device);
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

    WD_LOG_INFO("video encoder configured: backend=%s codec=%s size=%ux%u fps=%u bitrate_kib=%u%s%s",
                encoder->codec && encoder->codec->name ? encoder->codec->name : "unknown",
                wd_video_encoder_codec_name(config->codec), config->width, config->height,
                config->target_fps, config->bitrate_kib_per_second,
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

bool wd_video_encoder_request_keyframe(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return false;
    }

    encoder->keyframe_requested = true;
    return wd_video_encoder_available(encoder);
}

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
static const uint32_t* wd_video_encoder_prepare_xrgb_source(struct wd_video_encoder* encoder,
                                                           const struct wd_video_encoder_input_xrgb8888* input,
                                                           uint32_t coded_width,
                                                           uint32_t coded_height,
                                                           uint32_t* out_stride_pixels) {
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
        encoder->padded_pixels = new_pixels;
        encoder->padded_pixel_capacity = needed;
    }

    for (uint32_t y = 0; y < coded_height; ++y)
    {
        const uint32_t src_y = y < input->height ? y : input->height - 1u;
        const uint32_t* src = input->pixels + (size_t)src_y * input->stride_pixels;
        uint32_t* dst = encoder->padded_pixels + (size_t)y * coded_width;
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

static bool wd_video_encoder_prepare_frame(struct wd_video_encoder* encoder,
                                           const struct wd_video_encoder_input_xrgb8888* input) {
    uint32_t source_stride_pixels = 0;
    const uint32_t* source_pixels = wd_video_encoder_prepare_xrgb_source(
        encoder, input, (uint32_t)encoder->codec_ctx->width,
        (uint32_t)encoder->codec_ctx->height, &source_stride_pixels);
    if (!source_pixels)
    {
        return false;
    }

    const uint8_t* src_slices[4] = {(const uint8_t*)source_pixels, NULL, NULL, NULL};
    const int src_stride[4] = {(int)(source_stride_pixels * WD_BYTES_PER_PIXEL), 0, 0, 0};

    if (encoder->active_backend == WD_VIDEO_ENCODER_BACKEND_VAAPI)
    {
        if (!encoder->upload_frame || !encoder->vaapi_frames_ctx ||
            av_frame_make_writable(encoder->upload_frame) < 0)
        {
            return false;
        }

        if (sws_scale(encoder->sws_ctx, src_slices, src_stride, 0,
                      encoder->codec_ctx->height, encoder->upload_frame->data,
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
        if (sws_scale(encoder->sws_ctx, src_slices, src_stride, 0,
                      encoder->codec_ctx->height, encoder->frame->data,
                      encoder->frame->linesize) != encoder->frame->height)
        {
            return false;
        }
    }

    encoder->frame->pts = (int64_t)input->pts_usec;
    encoder->frame->pict_type = encoder->keyframe_requested
                                    ? AV_PICTURE_TYPE_I
                                    : AV_PICTURE_TYPE_NONE;
    return true;
}
#endif

bool wd_video_encoder_encode_xrgb8888(struct wd_video_encoder* encoder,
                                      const struct wd_video_encoder_input_xrgb8888* input,
                                      struct wd_video_encoder_packet* packet) {
    if (packet)
    {
        memset(packet, 0, sizeof(*packet));
    }

    if (!encoder || !input || !packet || !input->pixels || input->width == 0 ||
        input->height == 0 || input->stride_pixels < input->width)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER || WAYDISPLAY_HAVE_H264_SERVER_ENCODER
    if (!encoder->configured || !encoder->codec_ctx || !encoder->frame ||
        !encoder->packet || !encoder->sws_ctx ||
        input->width != encoder->config.width || input->height != encoder->config.height)
    {
        return false;
    }

    if (!wd_video_encoder_prepare_frame(encoder, input))
    {
        return false;
    }

    bool frame_sent = false;
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
                return false;
            }
            if (!have_output)
            {
                if (!wd_video_encoder_copy_packet(encoder, encoder->packet,
                                                  input->pts_usec, packet))
                {
                    av_packet_unref(encoder->packet);
                    return false;
                }
                have_output = true;
            }
        }
    }

    if (!frame_sent)
    {
        return false;
    }
    encoder->keyframe_requested = false;

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
            return false;
        }
        if (!have_output)
        {
            if (!wd_video_encoder_copy_packet(encoder, encoder->packet,
                                              input->pts_usec, packet))
            {
                av_packet_unref(encoder->packet);
                return false;
            }
            have_output = true;
        }
    }

    return true;
#else
    (void)encoder;
    (void)input;
    return false;
#endif
}
