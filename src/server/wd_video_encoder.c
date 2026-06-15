#include "wd_video_encoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef WAYDISPLAY_HAVE_H265_SERVER_ENCODER
#define WAYDISPLAY_HAVE_H265_SERVER_ENCODER 0
#endif

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#endif

struct wd_video_encoder {
    struct wd_video_encoder_config config;
    bool                           configured;
    bool                           keyframe_requested;
    uint64_t                       next_frame_id;

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    const AVCodec*     codec;
    AVCodecContext*    codec_ctx;
    AVFrame*           frame;
    AVPacket*          packet;
    struct SwsContext* sws_ctx;
    uint32_t*          padded_pixels;
    size_t             padded_pixel_capacity;
    uint8_t*           packet_copy;
    size_t             packet_copy_capacity;
#endif
};

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
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
    av_frame_free(&encoder->frame);
    avcodec_free_context(&encoder->codec_ctx);
}

static const AVCodec* wd_video_encoder_find_h265_codec(void) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    if (codec)
    {
        return codec;
    }

    return avcodec_find_encoder(AV_CODEC_ID_HEVC);
}

static bool wd_video_encoder_config_matches(const struct wd_video_encoder* encoder,
                                            const struct wd_video_encoder_config* config) {
    return encoder && config && encoder->configured && encoder->codec_ctx &&
           encoder->config.session_id == config->session_id && encoder->config.width == config->width &&
           encoder->config.height == config->height &&
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
    packet->header.codec      = WD_VIDEO_CODEC_H265;
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

bool wd_video_encoder_create(struct wd_video_encoder** out_encoder) {
    if (!out_encoder)
    {
        return false;
    }

    *out_encoder = calloc(1, sizeof(struct wd_video_encoder));
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (*out_encoder)
    {
        (*out_encoder)->codec = wd_video_encoder_find_h265_codec();
    }
#endif
    return *out_encoder != NULL;
}

void wd_video_encoder_destroy(struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    wd_video_encoder_release_backend(encoder);
#endif
    free(encoder);
}

void wd_video_encoder_reset(struct wd_video_encoder* encoder) {
    if (!encoder)
    {
        return;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    wd_video_encoder_release_backend(encoder);
    encoder->codec = wd_video_encoder_find_h265_codec();
#endif

    memset(&encoder->config, 0, sizeof(encoder->config));
    encoder->configured         = false;
    encoder->keyframe_requested = false;
    encoder->next_frame_id      = 0;
}

bool wd_video_encoder_available(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    return encoder && encoder->codec != NULL;
#else
    (void)encoder;
    return false;
#endif
}

const char* wd_video_encoder_backend_name(const struct wd_video_encoder* encoder) {
#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (encoder && encoder->codec && encoder->codec->name)
    {
        return encoder->codec->name;
    }
#endif
    (void)encoder;
    return "none";
}

bool wd_video_encoder_configure(struct wd_video_encoder* encoder, const struct wd_video_encoder_config* config) {
    if (!encoder || !config || config->codec != WD_VIDEO_CODEC_H265 || config->width == 0 || config->height == 0)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (!encoder->codec)
    {
        encoder->codec = wd_video_encoder_find_h265_codec();
    }
    if (!encoder->codec)
    {
        return false;
    }

    if (wd_video_encoder_config_matches(encoder, config))
    {
        return true;
    }

    wd_video_encoder_release_backend(encoder);

    encoder->codec_ctx = avcodec_alloc_context3(encoder->codec);
    encoder->frame     = av_frame_alloc();
    encoder->packet    = av_packet_alloc();
    if (!encoder->codec_ctx || !encoder->frame || !encoder->packet)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    const int fps = wd_video_encoder_effective_fps(config);
    encoder->codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    encoder->codec_ctx->codec_id   = AV_CODEC_ID_HEVC;
    encoder->codec_ctx->width      = wd_video_encoder_even_dimension(config->width);
    encoder->codec_ctx->height     = wd_video_encoder_even_dimension(config->height);
    encoder->codec_ctx->pix_fmt    = AV_PIX_FMT_YUV420P;
    encoder->codec_ctx->time_base  = (AVRational){1, 1000000};
    encoder->codec_ctx->framerate  = (AVRational){fps, 1};
    encoder->codec_ctx->gop_size   = fps > 0 ? fps : 30;
    encoder->codec_ctx->max_b_frames = 0;
    encoder->codec_ctx->thread_count = 2;
    encoder->codec_ctx->bit_rate   = wd_video_encoder_bitrate_bits_per_second(config);

    if (encoder->codec_ctx->priv_data)
    {
        (void)av_opt_set(encoder->codec_ctx->priv_data, "preset", "ultrafast", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "tune", "zerolatency", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "x265-params", "repeat-headers=1:log-level=error:pools=none:frame-threads=1", 0);
    }

    av_log_set_level(AV_LOG_WARNING);

    if (avcodec_open2(encoder->codec_ctx, encoder->codec, NULL) < 0)
    {
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

    encoder->sws_ctx = sws_getContext(encoder->codec_ctx->width, encoder->codec_ctx->height, AV_PIX_FMT_BGRA,
                                      encoder->codec_ctx->width, encoder->codec_ctx->height,
                                      encoder->codec_ctx->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!encoder->sws_ctx)
    {
        wd_video_encoder_release_backend(encoder);
        return false;
    }

    encoder->config             = *config;
    encoder->configured         = true;
    encoder->keyframe_requested = true;
    encoder->next_frame_id      = 1;
    return true;
#else
    encoder->config              = *config;
    encoder->configured          = true;
    encoder->keyframe_requested  = true;
    encoder->next_frame_id       = 1;

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


#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
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
#endif

bool wd_video_encoder_encode_xrgb8888(struct wd_video_encoder* encoder,
                                      const struct wd_video_encoder_input_xrgb8888* input,
                                      struct wd_video_encoder_packet* packet) {
    if (packet)
    {
        memset(packet, 0, sizeof(*packet));
    }

    if (!encoder || !input || !packet || !input->pixels || input->width == 0 || input->height == 0 ||
        input->stride_pixels < input->width)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_SERVER_ENCODER
    if (!encoder->configured || !encoder->codec_ctx || !encoder->frame || !encoder->packet || !encoder->sws_ctx ||
        input->width != encoder->config.width || input->height != encoder->config.height)
    {
        return false;
    }

    if (av_frame_make_writable(encoder->frame) < 0)
    {
        return false;
    }

    uint32_t source_stride_pixels = 0;
    const uint32_t* source_pixels = wd_video_encoder_prepare_xrgb_source(encoder, input,
                                                                         (uint32_t)encoder->codec_ctx->width,
                                                                         (uint32_t)encoder->codec_ctx->height,
                                                                         &source_stride_pixels);
    if (!source_pixels)
    {
        return false;
    }

    const uint8_t* src_slices[4] = {(const uint8_t*)source_pixels, NULL, NULL, NULL};
    const int      src_stride[4] = {(int)(source_stride_pixels * WD_BYTES_PER_PIXEL), 0, 0, 0};
    if (sws_scale(encoder->sws_ctx, src_slices, src_stride, 0, encoder->codec_ctx->height,
                  encoder->frame->data, encoder->frame->linesize) != encoder->frame->height)
    {
        return false;
    }

    encoder->frame->pts = (int64_t)input->pts_usec;
    encoder->frame->pict_type = encoder->keyframe_requested ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

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
                return false;
            }
            if (!have_output)
            {
                if (!wd_video_encoder_copy_packet(encoder, encoder->packet, input->pts_usec, packet))
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
            return false;
        }
        if (!have_output)
        {
            if (!wd_video_encoder_copy_packet(encoder, encoder->packet, input->pts_usec, packet))
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
