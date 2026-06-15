#include "video_decoder.hpp"

#include <cstring>
#include <new>
#include <vector>

#ifndef WAYDISPLAY_HAVE_H265_CLIENT_DECODER
#define WAYDISPLAY_HAVE_H265_CLIENT_DECODER 0
#endif

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace waydisplay {

struct ClientVideoDecoder {
    ClientVideoDecoderConfig config{};
    bool                     configured = false;
    std::vector<uint32_t>    pixels{};

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    const AVCodec*    codec     = nullptr;
    AVCodecContext*   codec_ctx = nullptr;
    AVFrame*          frame     = nullptr;
    AVPacket*         packet    = nullptr;
    SwsContext*       sws_ctx   = nullptr;
    AVPixelFormat     sws_src_format = AV_PIX_FMT_NONE;
#endif
};

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
void release_decoder_backend(ClientVideoDecoder* decoder) {
    if (!decoder)
    {
        return;
    }

    sws_freeContext(decoder->sws_ctx);
    decoder->sws_ctx = nullptr;

    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->frame);
    avcodec_free_context(&decoder->codec_ctx);
    decoder->sws_src_format = AV_PIX_FMT_NONE;
}

const AVCodec* find_h265_decoder() {
    return avcodec_find_decoder(AV_CODEC_ID_HEVC);
}

bool decoder_config_matches(const ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config) {
    return decoder && decoder->configured && decoder->codec_ctx && decoder->config.session_id == config.session_id &&
           decoder->config.width == config.width && decoder->config.height == config.height &&
           decoder->config.target_fps == config.target_fps && decoder->config.codec == config.codec;
}
#endif

bool client_video_decoder_create(ClientVideoDecoder** out_decoder) {
    if (!out_decoder)
    {
        return false;
    }

    *out_decoder = new (std::nothrow) ClientVideoDecoder();
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    if (*out_decoder)
    {
        (*out_decoder)->codec = find_h265_decoder();
    }
#endif
    return *out_decoder != nullptr;
}

void client_video_decoder_destroy(ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    release_decoder_backend(decoder);
#endif
    delete decoder;
}

void client_video_decoder_reset(ClientVideoDecoder* decoder) {
    if (!decoder)
    {
        return;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    release_decoder_backend(decoder);
    decoder->codec = find_h265_decoder();
#endif

    decoder->config     = ClientVideoDecoderConfig{};
    decoder->configured = false;
    decoder->pixels.clear();
}

bool client_video_decoder_available(const ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    return decoder && decoder->codec != nullptr;
#else
    (void)decoder;
    return false;
#endif
}

const char* client_video_decoder_backend_name(const ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    if (decoder && decoder->codec && decoder->codec->name)
    {
        return decoder->codec->name;
    }
#endif
    (void)decoder;
    return "none";
}

bool client_video_decoder_configure(ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config) {
    if (!decoder || config.codec != WD_VIDEO_CODEC_H265 || config.width == 0 || config.height == 0)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    if (!decoder->codec)
    {
        decoder->codec = find_h265_decoder();
    }
    if (!decoder->codec)
    {
        return false;
    }

    if (decoder_config_matches(decoder, config))
    {
        return true;
    }

    release_decoder_backend(decoder);

    decoder->codec_ctx = avcodec_alloc_context3(decoder->codec);
    decoder->frame     = av_frame_alloc();
    decoder->packet    = av_packet_alloc();
    if (!decoder->codec_ctx || !decoder->frame || !decoder->packet)
    {
        release_decoder_backend(decoder);
        return false;
    }

    decoder->codec_ctx->width  = config.width;
    decoder->codec_ctx->height = config.height;

    if (avcodec_open2(decoder->codec_ctx, decoder->codec, nullptr) < 0)
    {
        release_decoder_backend(decoder);
        return false;
    }

    try
    {
        decoder->pixels.assign(static_cast<size_t>(config.width) * config.height, 0xff000000u);
    }
    catch (...)
    {
        release_decoder_backend(decoder);
        decoder->pixels.clear();
        return false;
    }

    decoder->config     = config;
    decoder->configured = true;
    return true;
#else
    decoder->config     = config;
    decoder->configured = true;
    return client_video_decoder_available(decoder);
#endif
}

bool client_video_decoder_decode_h265(ClientVideoDecoder* decoder, const ClientVideoPacket& packet,
                                      ClientDecodedVideoFrame* out_frame) {
    if (out_frame)
    {
        *out_frame = ClientDecodedVideoFrame{};
    }

    if (!decoder || !decoder->configured || packet.header.codec != WD_VIDEO_CODEC_H265 || !packet.data ||
        packet.header.data_size == 0)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    if (!decoder->codec_ctx || !decoder->frame || !decoder->packet)
    {
        return false;
    }

    av_packet_unref(decoder->packet);
    if (av_new_packet(decoder->packet, static_cast<int>(packet.header.data_size)) < 0)
    {
        return false;
    }
    std::memcpy(decoder->packet->data, packet.data, packet.header.data_size);
    decoder->packet->pts = static_cast<int64_t>(packet.header.pts_usec);

    int rc = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
    av_packet_unref(decoder->packet);
    if (rc < 0)
    {
        return false;
    }

    rc = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
    if (rc < 0)
    {
        return false;
    }

    if (decoder->frame->width <= 0 || decoder->frame->height <= 0 ||
        packet.header.width == 0 || packet.header.height == 0)
    {
        av_frame_unref(decoder->frame);
        return false;
    }

    const auto src_format = static_cast<AVPixelFormat>(decoder->frame->format);
    decoder->sws_ctx = sws_getCachedContext(decoder->sws_ctx, decoder->frame->width, decoder->frame->height,
                                            src_format, packet.header.width, packet.header.height,
                                            AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!decoder->sws_ctx)
    {
        av_frame_unref(decoder->frame);
        return false;
    }
    decoder->sws_src_format = src_format;

    const size_t expected_pixels = static_cast<size_t>(packet.header.width) * packet.header.height;
    if (decoder->pixels.size() != expected_pixels)
    {
        try
        {
            decoder->pixels.assign(expected_pixels, 0xff000000u);
        }
        catch (...)
        {
            av_frame_unref(decoder->frame);
            decoder->pixels.clear();
            return false;
        }
    }

    uint8_t* const dst_slices[4] = {reinterpret_cast<uint8_t*>(decoder->pixels.data()), nullptr, nullptr, nullptr};
    const int      dst_stride[4] = {packet.header.width * static_cast<int>(sizeof(uint32_t)), 0, 0, 0};
    if (sws_scale(decoder->sws_ctx, decoder->frame->data, decoder->frame->linesize, 0, decoder->frame->height,
                  dst_slices, dst_stride) != packet.header.height)
    {
        av_frame_unref(decoder->frame);
        return false;
    }

    if (out_frame)
    {
        out_frame->pixels        = decoder->pixels.data();
        out_frame->width         = packet.header.width;
        out_frame->height        = packet.header.height;
        out_frame->stride_pixels = packet.header.width;
        out_frame->frame_id      = packet.header.frame_id;
        out_frame->pts_usec      = packet.header.pts_usec;
    }

    av_frame_unref(decoder->frame);
    return true;
#else
    (void)packet;
    return client_video_decoder_available(decoder);
#endif
}

} // namespace waydisplay
