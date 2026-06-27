#include "video_decoder.hpp"

#include "waydisplay/wd_log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
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

#include "../common/wd_vaapi_device.h"
#endif

namespace waydisplay {

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
namespace {

struct SubmittedFrameMetadata {
    wd_video_frame_payload_header header{};
    int64_t                       codec_pts = AV_NOPTS_VALUE;
};

struct QueuedDecodedFrame {
    ClientDecodedVideoFrame metadata{};
    ClientVideoFrameBuffer  buffer{};
};

} // namespace
#endif

struct ClientVideoDecoder {
    ClientVideoDecoderConfig config{};
    bool                     configured = false;
    ClientVideoFrameBuffer   output{};
    ClientDecodedVideoFrame  output_metadata{};
    bool                     output_ready = false;

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    const AVCodec*                      codec                = nullptr;
    AVCodecContext*                     codec_ctx            = nullptr;
    AVFrame*                            frame                = nullptr;
    AVFrame*                            sw_frame             = nullptr;
    AVPacket*                           packet               = nullptr;
    SwsContext*                         sws_ctx              = nullptr;
    AVBufferRef*                        hw_device_ctx        = nullptr;
    AVPixelFormat                       sws_src_format       = AV_PIX_FMT_NONE;
    bool                                vaapi_requested      = false;
    bool                                vaapi_required       = false;
    bool                                using_vaapi          = false;
    bool                                vaapi_auto_disabled  = false;
    bool                                vaapi_disable_logged = false;
    std::vector<SubmittedFrameMetadata> submitted_frames{};
    std::deque<QueuedDecodedFrame>      decoded_frames{};
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
    decoder->sws_src_format  = AV_PIX_FMT_NONE;
    decoder->vaapi_requested = false;
    decoder->vaapi_required  = false;
    decoder->using_vaapi     = false;
    decoder->submitted_frames.clear();
    decoder->decoded_frames.clear();
    decoder->output_metadata = ClientDecodedVideoFrame{};
    decoder->output_ready    = false;
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
    return decoder && decoder->configured && decoder->codec_ctx && decoder->config.session_id == config.session_id &&
           decoder->config.connection_token == config.connection_token && decoder->config.content_epoch == config.content_epoch &&
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
    decoder->using_vaapi         = false;
    if (!decoder->vaapi_disable_logged)
    {
        WD_LOG_WARN("VAAPI video decode failed%s%s; falling back to software decode", reason && *reason ? ": " : "",
                    reason && *reason ? reason : "");
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

bool convert_decoder_frame(ClientVideoDecoder* decoder, const wd_video_frame_payload_header& header, ClientVideoFrameBuffer* output,
                           ClientDecodedVideoFrame* out_frame) {
    if (!decoder || !decoder->frame || !output || !out_frame) [[unlikely]]
    {
        return false;
    }

    AVFrame* src_frame = decoder->frame;
    if (!transfer_hw_frame_if_needed(decoder, &src_frame)) [[unlikely]]
    {
        return false;
    }

    if (src_frame->width <= 0 || src_frame->height <= 0 || header.width == 0 || header.height == 0) [[unlikely]]
    {
        return false;
    }

    const uint16_t coded_width  = header.coded_width != 0 ? header.coded_width : static_cast<uint16_t>(src_frame->width);
    const uint16_t coded_height = header.coded_height != 0 ? header.coded_height : static_cast<uint16_t>(src_frame->height);
    if (coded_width < header.width || coded_height < header.height || src_frame->width < coded_width || src_frame->height < coded_height) [[unlikely]]
    {
        return false;
    }

    /* Keep decoded video in planar 4:2:0. SDL can upload this directly to an
     * IYUV texture, avoiding BGRA conversion and cutting CPU-to-GPU upload
     * traffic from four bytes to roughly one and a half bytes per pixel. */
    const int  visible_width  = static_cast<int>(header.width);
    const int  visible_height = static_cast<int>(header.height);
    const auto src_format     = static_cast<AVPixelFormat>(src_frame->format);
    decoder->sws_ctx = sws_getCachedContext(decoder->sws_ctx, visible_width, visible_height, src_format, visible_width, visible_height,
                                            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!decoder->sws_ctx) [[unlikely]]
    {
        return false;
    }
    decoder->sws_src_format = src_format;

    const uint32_t y_pitch        = header.width;
    const uint32_t uv_width       = (header.width + 1u) / 2u;
    const uint32_t uv_height      = (header.height + 1u) / 2u;
    const size_t   y_size         = static_cast<size_t>(y_pitch) * header.height;
    const size_t   uv_size        = static_cast<size_t>(uv_width) * uv_height;
    const size_t   expected_bytes = y_size + uv_size * 2u;
    try
    {
        if (output->bytes.size() != expected_bytes)
        {
            output->bytes.resize(expected_bytes);
        }
    }
    catch (...)
    {
        output->clear();
        return false;
    }

    output->format   = ClientVideoPixelFormat::IYUV;
    output->width    = header.width;
    output->height   = header.height;
    output->y_pitch  = y_pitch;
    output->uv_pitch = uv_width;
    output->u_offset = y_size;
    output->v_offset = y_size + uv_size;

    uint8_t* const dst_slices[4] = {
        output->bytes.data(),
        output->bytes.data() + output->u_offset,
        output->bytes.data() + output->v_offset,
        nullptr,
    };
    const int dst_stride[4] = {static_cast<int>(y_pitch), static_cast<int>(uv_width), static_cast<int>(uv_width), 0};
    if (sws_scale(decoder->sws_ctx, src_frame->data, src_frame->linesize, 0, visible_height, dst_slices, dst_stride) != visible_height) [[unlikely]]
    {
        return false;
    }

    out_frame->format        = ClientVideoPixelFormat::IYUV;
    out_frame->width         = header.width;
    out_frame->height        = header.height;
    out_frame->frame_id      = header.frame_id;
    out_frame->content_epoch = header.content_epoch;
    out_frame->pts_usec      = header.pts_usec;

    return true;
}

bool take_submitted_metadata(ClientVideoDecoder* decoder, wd_video_frame_payload_header* header) {
    if (!decoder || !decoder->frame || !header || decoder->submitted_frames.empty())
    {
        return false;
    }

    auto          metadata     = decoder->submitted_frames.end();
    const int64_t timestamps[] = {
        decoder->frame->pts,
        decoder->frame->best_effort_timestamp,
    };
    for (const int64_t codec_pts : timestamps)
    {
        if (codec_pts == AV_NOPTS_VALUE)
        {
            continue;
        }
        metadata = std::find_if(decoder->submitted_frames.begin(), decoder->submitted_frames.end(),
                                [codec_pts](const SubmittedFrameMetadata& candidate) { return candidate.codec_pts == codec_pts; });
        if (metadata != decoder->submitted_frames.end())
        {
            break;
        }
    }
    if (metadata == decoder->submitted_frames.end())
    {
        if (decoder->submitted_frames.size() != 1)
        {
            return false;
        }
        metadata = decoder->submitted_frames.begin();
    }

    *header = metadata->header;
    decoder->submitted_frames.erase(metadata);
    return true;
}

bool queue_decoder_frame(ClientVideoDecoder* decoder) {
    if (!decoder || decoder->decoded_frames.size() >= WD_CLIENT_VIDEO_DECODED_QUEUE_CAPACITY) [[unlikely]]
    {
        return false;
    }

    wd_video_frame_payload_header header{};
    if (!take_submitted_metadata(decoder, &header))
    {
        return false;
    }

    QueuedDecodedFrame queued{};
    if (decoder->decoded_frames.empty() && !decoder->output_ready)
    {
        queued.buffer = std::move(decoder->output);
    }
    if (!convert_decoder_frame(decoder, header, &queued.buffer, &queued.metadata))
    {
        return false;
    }

    try
    { decoder->decoded_frames.push_back(std::move(queued)); }
    catch (...)
    { return false; }
    return true;
}

bool receive_decoder_frames(ClientVideoDecoder* decoder) {
    if (!decoder || !decoder->codec_ctx || !decoder->frame)
    {
        return false;
    }

    for (;;)
    {
        av_frame_unref(decoder->frame);
        const int rc = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) [[likely]]
        {
            return true;
        }
        if (rc < 0) [[unlikely]]
        {
            mark_vaapi_auto_failed(decoder, "hardware frame receive failed");
            return false;
        }
        if (!queue_decoder_frame(decoder)) [[unlikely]]
        {
            av_frame_unref(decoder->frame);
            return false;
        }
    }
}

bool activate_next_output(ClientVideoDecoder* decoder, ClientDecodedVideoFrame* out_frame) {
    if (!decoder || decoder->output_ready || decoder->decoded_frames.empty())
    {
        return false;
    }

    QueuedDecodedFrame queued = std::move(decoder->decoded_frames.front());
    decoder->decoded_frames.pop_front();
    decoder->output          = std::move(queued.buffer);
    decoder->output_metadata = queued.metadata;
    decoder->output_ready    = true;
    if (out_frame)
    {
        *out_frame = decoder->output_metadata;
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
        try
        { (*out_decoder)->submitted_frames.reserve(WD_CLIENT_VIDEO_METADATA_QUEUE_CAPACITY); }
        catch (...)
        {
            delete *out_decoder;
            *out_decoder = nullptr;
        }
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
    decoder->output_metadata = ClientDecodedVideoFrame{};
    decoder->output_ready    = false;
}

bool client_video_decoder_swap_output_frame(ClientVideoDecoder* decoder, ClientVideoFrameBuffer& frame) {
    if (!decoder || !decoder->output_ready || !decoder->output.valid())
    {
        return false;
    }

    std::swap(frame, decoder->output);
    decoder->output_metadata = ClientDecodedVideoFrame{};
    decoder->output_ready    = false;
    return true;
}

bool client_video_decoder_take_frame(ClientVideoDecoder* decoder, ClientDecodedVideoFrame* out_frame) {
    if (out_frame)
    {
        *out_frame = ClientDecodedVideoFrame{};
    }
#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    return activate_next_output(decoder, out_frame);
#else
    (void)decoder;
    return false;
#endif
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
    if (!decoder || (config.codec != WD_VIDEO_CODEC_H265 && config.codec != WD_VIDEO_CODEC_H264) || config.width == 0 ||
        config.height == 0 || config.coded_width < config.width || config.coded_height < config.height) [[unlikely]]
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    decoder->codec = find_decoder_for_codec(config.codec);
    if (!decoder->codec)
    {
        return false;
    }

    if (decoder_config_matches(decoder, config)) [[likely]]
    {
        return true;
    }

    release_decoder_backend(decoder);

    decoder->codec_ctx = avcodec_alloc_context3(decoder->codec);
    decoder->frame     = av_frame_alloc();
    decoder->packet    = av_packet_alloc();
    if (!decoder->codec_ctx || !decoder->frame || !decoder->packet) [[unlikely]]
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
    decoder->vaapi_requested =
        config.hwdecode_mode != WD_CLIENT_VIDEO_HWDECODE_OFF && (decoder->vaapi_required || !decoder->vaapi_auto_disabled);
    if (decoder->vaapi_requested)
    {
        char selected_device[PATH_MAX] = {};
        if (wd_vaapi_open_automatic_device(&decoder->hw_device_ctx, selected_device, sizeof(selected_device)) >= 0)
        {
            WD_LOG_INFO("VAAPI video decode device initialized: %s", selected_device);
            decoder->codec_ctx->hw_device_ctx = av_buffer_ref(decoder->hw_device_ctx);
            if (!decoder->codec_ctx->hw_device_ctx)
            {
                release_decoder_backend(decoder);
                return false;
            }
            decoder->codec_ctx->opaque     = decoder;
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

    if (avcodec_open2(decoder->codec_ctx, decoder->codec, nullptr) < 0) [[unlikely]]
    {
#if WAYDISPLAY_HAVE_VAAPI_CLIENT_DECODER
        const bool retry_software =
            decoder->vaapi_requested && !decoder->vaapi_required && mark_vaapi_auto_failed(decoder, "hardware decoder open failed");
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
    decoder->output_metadata = ClientDecodedVideoFrame{};
    decoder->output_ready    = false;

    decoder->config     = config;
    decoder->configured = true;
    return true;
#else
    decoder->config     = config;
    decoder->configured = true;
    return client_video_decoder_available(decoder);
#endif
}

bool client_video_decoder_decode(ClientVideoDecoder* decoder, const ClientVideoPacket& packet, ClientDecodedVideoFrame* out_frame) {
    if (out_frame)
    {
        *out_frame = ClientDecodedVideoFrame{};
    }

    if (!decoder || !decoder->configured || (packet.header.codec != WD_VIDEO_CODEC_H265 && packet.header.codec != WD_VIDEO_CODEC_H264) ||
        !packet.data || packet.header.data_size == 0) [[unlikely]]
    {
        return false;
    }

#if WAYDISPLAY_HAVE_H265_CLIENT_DECODER || WAYDISPLAY_HAVE_H264_CLIENT_DECODER
    if (!decoder->codec_ctx || !decoder->frame || !decoder->packet || decoder->output_ready ||
        packet.header.pts_usec > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        return false;
    }

    av_packet_unref(decoder->packet);
    if (av_new_packet(decoder->packet, static_cast<int>(packet.header.data_size)) < 0) [[unlikely]]
    {
        return false;
    }
    std::memcpy(decoder->packet->data, packet.data, packet.header.data_size);
    const int64_t codec_pts = static_cast<int64_t>(packet.header.pts_usec);
    decoder->packet->pts    = codec_pts;

    if (decoder->submitted_frames.size() >= WD_CLIENT_VIDEO_METADATA_QUEUE_CAPACITY) [[unlikely]]
    {
        av_packet_unref(decoder->packet);
        return false;
    }

    bool sent_packet = false;

    for (int attempt = 0; attempt < 2 && !sent_packet; ++attempt)
    {
        int rc = avcodec_send_packet(decoder->codec_ctx, decoder->packet);
        if (rc == 0) [[likely]]
        {
            SubmittedFrameMetadata metadata{};
            metadata.header    = packet.header;
            metadata.codec_pts = codec_pts;
            decoder->submitted_frames.push_back(metadata);
            sent_packet = true;
            break;
        }
        if (rc != AVERROR(EAGAIN))
        {
            mark_vaapi_auto_failed(decoder, "hardware packet submit failed");
            av_packet_unref(decoder->packet);
            return false;
        }

        if (!receive_decoder_frames(decoder)) [[unlikely]]
        {
            av_packet_unref(decoder->packet);
            return false;
        }
    }

    av_packet_unref(decoder->packet);
    if (!sent_packet) [[unlikely]]
    {
        return false;
    }

    if (!receive_decoder_frames(decoder))
    {
        return false;
    }

    (void)activate_next_output(decoder, out_frame);
    return true;
#else
    (void)packet;
    return client_video_decoder_available(decoder);
#endif
}

} // namespace waydisplay
