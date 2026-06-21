#include "video_decoder.hpp"

#include "waydisplay/wd_log.h"

#include <cstring>
#include <cstdlib>
#include <new>
#include <vector>

#ifndef WAYDISPLAY_HAVE_H265_CLIENT_DECODER
#define WAYDISPLAY_HAVE_H265_CLIENT_DECODER 0
#endif

#ifndef WAYDISPLAY_HAVE_H264_CLIENT_DECODER
#define WAYDISPLAY_HAVE_H264_CLIENT_DECODER 0
#endif

#ifndef WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
#define WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER 0
#endif

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace waydisplay {

struct ClientVideoDecoder {
    ClientVideoDecoderConfig config{};
    bool                     configured = false;
    ClientVideoFrameBuffer   output{};

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    const AVCodec*    codec     = nullptr;
    AVCodecContext*   codec_ctx = nullptr;
    AVFrame*          frame     = nullptr;
    AVFrame*          sw_frame  = nullptr;
    AVPacket*         packet    = nullptr;
    SwsContext*       sws_ctx   = nullptr;
    AVBufferRef*      hw_device_ctx = nullptr;
    AVPixelFormat     sws_src_format = AV_PIX_FMT_NONE;
    bool              vaapi_requested = false;
    bool              vaapi_required = false;
    bool              using_vaapi = false;
    bool              vaapi_auto_disabled = false;
    bool              vaapi_disable_logged = false;
#endif
};

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
void release_decoder_backend(ClientVideoDecoder* decoder) {
    if (!decoder)
    {
        return;
    }

    sws_freeContext(decoder->sws_ctx);
    decoder->sws_ctx = nullptr;

    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->sw_frame);
    av_frame_free(&decoder->frame);
    avcodec_free_context(&decoder->codec_ctx);
    av_buffer_unref(&decoder->hw_device_ctx);
    decoder->sws_src_format = AV_PIX_FMT_NONE;
    decoder->vaapi_requested = false;
    decoder->vaapi_required = false;
    decoder->using_vaapi = false;
}

const AVCodec* find_decoder_for_codec(uint32_t codec) {
    switch (codec)
    {
#if WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    case WD_VIDEO_CODEC_H264:
        return avcodec_find_decoder(AV_CODEC_ID_H264);
#endif
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    case WD_VIDEO_CODEC_H265:
        return avcodec_find_decoder(AV_CODEC_ID_HEVC);
#endif
    default:
        return nullptr;
    }
}

uint32_t supported_decoder_codecs() {
    uint32_t codecs = 0;
#if WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    if (avcodec_find_decoder(AV_CODEC_ID_H264))
    {
        codecs |= WD_VIDEO_CODEC_H264;
    }
#endif
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER
    if (avcodec_find_decoder(AV_CODEC_ID_HEVC))
    {
        codecs |= WD_VIDEO_CODEC_H265;
    }
#endif
    return codecs;
}


bool decoder_config_matches(const ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config) {
    return decoder && decoder->configured && decoder->codec_ctx && decoder->config.session_id == config.session_id && decoder->config.connection_token == config.connection_token && decoder->config.content_epoch == config.content_epoch &&
           decoder->config.width == config.width && decoder->config.height == config.height &&
           decoder->config.coded_width == config.coded_width && decoder->config.coded_height == config.coded_height &&
           decoder->config.target_fps == config.target_fps && decoder->config.codec == config.codec &&
           decoder->config.hwdecode_mode == config.hwdecode_mode;
}

#if WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
AVPixelFormat select_decoder_hw_format(AVCodecContext* codec_ctx, const AVPixelFormat* formats) {
    auto* decoder = codec_ctx ? static_cast<ClientVideoDecoder*>(codec_ctx->opaque) : nullptr;
    for (const AVPixelFormat* fmt = formats; fmt && *fmt != AV_PIX_FMT_NONE; ++fmt)
    {
        if (*fmt == AV_PIX_FMT_VAAPI)
        {
            if (decoder)
            {
                decoder->using_vaapi = true;
            }
            return *fmt;
        }
    }

    if (decoder)
    {
        decoder->using_vaapi = false;
    }
    return formats && *formats != AV_PIX_FMT_NONE ? *formats : AV_PIX_FMT_NONE;
}
#endif

bool mark_vaapi_auto_failed(ClientVideoDecoder* decoder, const char* reason) {
#if WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
    if (!decoder || decoder->vaapi_required || !decoder->vaapi_requested)
    {
        return false;
    }

    decoder->vaapi_auto_disabled = true;
    decoder->using_vaapi = false;
    if (!decoder->vaapi_disable_logged)
    {
        WD_LOG_WARN("VAAPI video decode failed%s%s; falling back to software decode",
                    reason && *reason ? ": " : "", reason && *reason ? reason : "");
        decoder->vaapi_disable_logged = true;
    }
    return true;
#else
    (void)decoder;
    (void)reason;
    return false;
#endif
}

bool transfer_hw_frame_if_needed(ClientVideoDecoder* decoder, AVFrame** frame) {
    if (!decoder || !frame || !*frame)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
    if ((*frame)->format == AV_PIX_FMT_VAAPI)
    {
        if (!decoder->sw_frame)
        {
            decoder->sw_frame = av_frame_alloc();
            if (!decoder->sw_frame)
            {
                mark_vaapi_auto_failed(decoder, "software transfer frame allocation failed");
                return false;
            }
        }
        av_frame_unref(decoder->sw_frame);
        if (av_hwframe_transfer_data(decoder->sw_frame, *frame, 0) < 0)
        {
            mark_vaapi_auto_failed(decoder, "hardware frame transfer failed");
            return false;
        }
        *frame = decoder->sw_frame;
    }
#endif

    return true;
}

bool convert_decoder_frame(ClientVideoDecoder* decoder, const ClientVideoPacket& packet,
                           ClientDecodedVideoFrame* out_frame) {
    if (!decoder || !decoder->frame)
    {
        return false;
    }

    AVFrame* src_frame = decoder->frame;
    if (!transfer_hw_frame_if_needed(decoder, &src_frame))
    {
        return false;
    }

    if (src_frame->width <= 0 || src_frame->height <= 0 ||
        packet.header.width == 0 || packet.header.height == 0)
    {
        return false;
    }

    const uint16_t coded_width = packet.header.coded_width != 0
                                     ? packet.header.coded_width
                                     : static_cast<uint16_t>(src_frame->width);
    const uint16_t coded_height = packet.header.coded_height != 0
                                      ? packet.header.coded_height
                                      : static_cast<uint16_t>(src_frame->height);
    if (coded_width < packet.header.width || coded_height < packet.header.height ||
        src_frame->width < coded_width || src_frame->height < coded_height)
    {
        return false;
    }

    /* Keep decoded video in planar 4:2:0. SDL can upload this directly to an
     * IYUV texture, avoiding BGRA conversion and cutting CPU-to-GPU upload
     * traffic from four bytes to roughly one and a half bytes per pixel. */
    const int visible_width = static_cast<int>(packet.header.width);
    const int visible_height = static_cast<int>(packet.header.height);
    const auto src_format = static_cast<AVPixelFormat>(src_frame->format);
    decoder->sws_ctx = sws_getCachedContext(decoder->sws_ctx, visible_width, visible_height,
                                            src_format, visible_width, visible_height,
                                            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!decoder->sws_ctx)
    {
        return false;
    }
    decoder->sws_src_format = src_format;

    const uint32_t y_pitch = packet.header.width;
    const uint32_t uv_width = (packet.header.width + 1u) / 2u;
    const uint32_t uv_height = (packet.header.height + 1u) / 2u;
    const size_t y_size = static_cast<size_t>(y_pitch) * packet.header.height;
    const size_t uv_size = static_cast<size_t>(uv_width) * uv_height;
    const size_t expected_bytes = y_size + uv_size * 2u;
    try
    {
        if (decoder->output.bytes.size() != expected_bytes)
        {
            decoder->output.bytes.resize(expected_bytes);
        }
    }
    catch (...)
    {
        decoder->output.clear();
        return false;
    }

    decoder->output.format = ClientVideoPixelFormat::IYUV;
    decoder->output.width = packet.header.width;
    decoder->output.height = packet.header.height;
    decoder->output.y_pitch = y_pitch;
    decoder->output.uv_pitch = uv_width;
    decoder->output.u_offset = y_size;
    decoder->output.v_offset = y_size + uv_size;

    uint8_t* const dst_slices[4] = {
        decoder->output.bytes.data(),
        decoder->output.bytes.data() + decoder->output.u_offset,
        decoder->output.bytes.data() + decoder->output.v_offset,
        nullptr,
    };
    const int dst_stride[4] = {static_cast<int>(y_pitch), static_cast<int>(uv_width),
                               static_cast<int>(uv_width), 0};
    if (sws_scale(decoder->sws_ctx, src_frame->data, src_frame->linesize, 0, visible_height,
                  dst_slices, dst_stride) != visible_height)
    {
        return false;
    }

    if (out_frame)
    {
        out_frame->format        = ClientVideoPixelFormat::IYUV;
        out_frame->width         = packet.header.width;
        out_frame->height        = packet.header.height;
        out_frame->frame_id      = packet.header.frame_id;
        out_frame->content_epoch = packet.header.content_epoch;
        out_frame->pts_usec      = packet.header.pts_usec;
    }

    return true;
}
#endif

bool client_video_decoder_create(ClientVideoDecoder** out_decoder) {
    if (!out_decoder)
    {
        return false;
    }

    *out_decoder = new (std::nothrow) ClientVideoDecoder();
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    if (*out_decoder)
    {
        (*out_decoder)->codec = nullptr;
    }
#endif
    return *out_decoder != nullptr;
}

void client_video_decoder_destroy(ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    release_decoder_backend(decoder);
#endif
    delete decoder;
}

void client_video_decoder_reset(ClientVideoDecoder* decoder) {
    if (!decoder)
    {
        return;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    release_decoder_backend(decoder);
    decoder->codec = nullptr;
#endif

    decoder->config     = ClientVideoDecoderConfig{};
    decoder->configured = false;
    decoder->output.clear();
}

bool client_video_decoder_swap_output_frame(ClientVideoDecoder* decoder, ClientVideoFrameBuffer& frame) {
    if (!decoder || !decoder->output.valid())
    {
        return false;
    }

    std::swap(frame, decoder->output);
    return true;
}

bool client_video_decoder_available(const ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    return decoder && supported_decoder_codecs() != 0;
#else
    (void)decoder;
    return false;
#endif
}

uint32_t client_video_decoder_supported_codecs(const ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    return decoder ? supported_decoder_codecs() : 0;
#else
    (void)decoder;
    return 0;
#endif
}

const char* client_video_decoder_backend_name(const ClientVideoDecoder* decoder) {
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    if (decoder && decoder->configured && decoder->using_vaapi)
    {
        return decoder->config.codec == WD_VIDEO_CODEC_H264 ? "h264+vaapi" : "hevc+vaapi";
    }
    if (decoder && decoder->codec && decoder->codec->name)
    {
        return decoder->codec->name;
    }
    if (supported_decoder_codecs() == (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265))
    {
        return "h264/hevc";
    }
    if ((supported_decoder_codecs() & WD_VIDEO_CODEC_H264) != 0)
    {
        return "h264";
    }
    if ((supported_decoder_codecs() & WD_VIDEO_CODEC_H265) != 0)
    {
        return "hevc";
    }
#endif
    (void)decoder;
    return "none";
}

bool client_video_decoder_hwdecode_failed_auto(const ClientVideoDecoder* decoder) {
#if (WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER) && WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
    return decoder && decoder->vaapi_auto_disabled;
#else
    (void)decoder;
    return false;
#endif
}

bool client_video_decoder_configure(ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config) {
    if (!decoder || (config.codec != WD_VIDEO_CODEC_H265 && config.codec != WD_VIDEO_CODEC_H264) || config.width == 0 || config.height == 0 ||
        config.coded_width < config.width || config.coded_height < config.height)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    decoder->codec = find_decoder_for_codec(config.codec);
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

    decoder->codec_ctx->width  = config.coded_width != 0 ? config.coded_width : config.width;
    decoder->codec_ctx->height = config.coded_height != 0 ? config.coded_height : config.height;
    decoder->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decoder->codec_ctx->thread_count = 1;

#if WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
    decoder->vaapi_required = config.hwdecode_mode == WD_CLIENT_VIDEO_HWDECODE_VAAPI;
    decoder->vaapi_requested = config.hwdecode_mode != WD_CLIENT_VIDEO_HWDECODE_OFF &&
                               (decoder->vaapi_required || !decoder->vaapi_auto_disabled);
    if (decoder->vaapi_requested)
    {
        const AVHWDeviceType vaapi_type = av_hwdevice_find_type_by_name("vaapi");
        const char* vaapi_device = std::getenv("WAYDISPLAY_VAAPI_DEVICE");
        if (vaapi_type != AV_HWDEVICE_TYPE_NONE &&
            av_hwdevice_ctx_create(&decoder->hw_device_ctx, vaapi_type, vaapi_device && *vaapi_device ? vaapi_device : nullptr,
                                   nullptr, 0) >= 0)
        {
            decoder->codec_ctx->hw_device_ctx = av_buffer_ref(decoder->hw_device_ctx);
            if (!decoder->codec_ctx->hw_device_ctx)
            {
                release_decoder_backend(decoder);
                return false;
            }
            decoder->codec_ctx->opaque = decoder;
            decoder->codec_ctx->get_format = select_decoder_hw_format;
        }
        else if (decoder->vaapi_required)
        {
            release_decoder_backend(decoder);
            return false;
        }
    }
#else
    if (config.hwdecode_mode == WD_CLIENT_VIDEO_HWDECODE_VAAPI)
    {
        release_decoder_backend(decoder);
        return false;
    }
#endif

    if (avcodec_open2(decoder->codec_ctx, decoder->codec, nullptr) < 0)
    {
#if WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
        const bool retry_software = decoder->vaapi_requested && !decoder->vaapi_required &&
                                    mark_vaapi_auto_failed(decoder, "hardware decoder open failed");
        if (retry_software)
        {
            release_decoder_backend(decoder);
            return client_video_decoder_configure(decoder, config);
        }
#endif
        release_decoder_backend(decoder);
        return false;
    }

    decoder->output.clear();

    decoder->config     = config;
    decoder->configured = true;
    return true;
#else
    decoder->config     = config;
    decoder->configured = true;
    return client_video_decoder_available(decoder);
#endif
}

bool client_video_decoder_decode(ClientVideoDecoder* decoder, const ClientVideoPacket& packet,
                                 ClientDecodedVideoFrame* out_frame) {
    if (out_frame)
    {
        *out_frame = ClientDecodedVideoFrame{};
    }

    if (!decoder || !decoder->configured || (packet.header.codec != WD_VIDEO_CODEC_H265 && packet.header.codec != WD_VIDEO_CODEC_H264) || !packet.data ||
        packet.header.data_size == 0)
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
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

    bool sent_packet = false;
    bool got_frame = false;
    bool converted_frame = false;

    for (int attempt = 0; attempt < 2 && !sent_packet; ++attempt)
    {
        int rc = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
        if (rc == 0)
        {
            sent_packet = true;
            break;
        }
        if (rc != AVERROR(EAGAIN))
        {
            mark_vaapi_auto_failed(decoder, "hardware packet submit failed");
            av_packet_unref(decoder->packet);
            return false;
        }

        for (;;)
        {
            av_frame_unref(decoder->frame);
            rc = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
            {
                break;
            }
            if (rc < 0)
            {
                mark_vaapi_auto_failed(decoder, "hardware frame receive failed");
                av_packet_unref(decoder->packet);
                return false;
            }
            got_frame = true;
            converted_frame = convert_decoder_frame(decoder, packet, out_frame);
            av_frame_unref(decoder->frame);
            if (!converted_frame)
            {
                av_packet_unref(decoder->packet);
                return false;
            }
        }
    }

    av_packet_unref(decoder->packet);
    if (!sent_packet)
    {
        return false;
    }

    for (;;)
    {
        av_frame_unref(decoder->frame);
        int rc = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
        {
            break;
        }
        if (rc < 0)
        {
            mark_vaapi_auto_failed(decoder, "hardware frame receive failed");
            return false;
        }
        got_frame = true;
        converted_frame = convert_decoder_frame(decoder, packet, out_frame);
        av_frame_unref(decoder->frame);
        if (!converted_frame)
        {
            return false;
        }
    }

    return got_frame ? converted_frame : true;
#else
    (void)packet;
    return client_video_decoder_available(decoder);
#endif
}

} // namespace waydisplay
