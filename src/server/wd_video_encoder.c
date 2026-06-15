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
    encoder->codec_ctx->bit_rate   = wd_video_encoder_bitrate_bits_per_second(config);

    if (encoder->codec_ctx->priv_data)
    {
        (void)av_opt_set(encoder->codec_ctx->priv_data, "preset", "ultrafast", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "tune", "zerolatency", 0);
        (void)av_opt_set(encoder->codec_ctx->priv_data, "x265-params", "repeat-headers=1", 0);
    }

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

    encoder->sws_ctx = sws_getContext(config->width, config->height, AV_PIX_FMT_BGRA,
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

    const uint8_t* src_slices[4] = {(const uint8_t*)input->pixels, NULL, NULL, NULL};
    const int      src_stride[4] = {(int)(input->stride_pixels * WD_BYTES_PER_PIXEL), 0, 0, 0};
    if (sws_scale(encoder->sws_ctx, src_slices, src_stride, 0, input->height,
                  encoder->frame->data, encoder->frame->linesize) != encoder->frame->height)
    {
        return false;
    }

    encoder->frame->pts = (int64_t)input->pts_usec;
    encoder->frame->pict_type = encoder->keyframe_requested ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    av_packet_unref(encoder->packet);
    int rc = avcodec_send_frame(encoder->codec_ctx, encoder->frame);
    if (rc < 0)
    {
        return false;
    }

    rc = avcodec_receive_packet(encoder->codec_ctx, encoder->packet);
    if (rc < 0)
    {
        return false;
    }

    if (encoder->packet->size <= 0 || (uint64_t)encoder->packet->size > WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES)
    {
        av_packet_unref(encoder->packet);
        return false;
    }

    packet->header.session_id = encoder->config.session_id;
    packet->header.codec      = WD_VIDEO_CODEC_H265;
    packet->header.flags      = 0;
    if ((encoder->packet->flags & AV_PKT_FLAG_KEY) != 0)
    {
        packet->header.flags |= WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_CONFIG;
    }
    packet->header.frame_id   = encoder->next_frame_id++;
    packet->header.pts_usec   = input->pts_usec;
    packet->header.width      = (uint16_t)input->width;
    packet->header.height     = (uint16_t)input->height;
    packet->header.data_size  = (uint32_t)encoder->packet->size;
    packet->data              = encoder->packet->data;

    encoder->keyframe_requested = false;
    return true;
#else
    (void)encoder;
    (void)input;
    return false;
#endif
}
