#pragma once

#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_buffer.h"
#include "waydisplay/wd_frame.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace waydisplay {

struct ClientVideoDecoder;

struct ClientVideoDecoderConfig {
    uint8_t  session_id       = 0;
    uint64_t connection_token = 0;
    uint64_t content_epoch    = 0;
    uint16_t width            = 0;
    uint16_t height           = 0;
    uint16_t coded_width      = 0;
    uint16_t coded_height     = 0;
    uint16_t target_fps       = 0;
    uint32_t codec            = 0;
    uint8_t  decode_mode       = WD_CLIENT_VIDEO_DECODER_AUTO;
    bool     prefer_gpu_output = false;
};

struct ClientVideoPacket {
    wd_video_frame_payload_header header{};
    const uint8_t*                data  = nullptr;
    /* Non-owning pointer to the storage that contains data. The caller keeps
     * one reference for the duration of decode(); the decoder retains its own
     * reference while FFmpeg can observe the packet. Null preserves the
     * legacy copy path for standalone callers/tests. */
    wd_buffer*                    owner = nullptr;
};

enum class ClientVideoPixelFormat : uint8_t {
    None = 0,
    IYUV = 1,
    DRMPrime = 2,
};

struct ClientVideoFrameBuffer {
    ClientVideoPixelFormat format   = ClientVideoPixelFormat::None;
    uint32_t               width    = 0;
    uint32_t               height   = 0;
    uint32_t               y_pitch  = 0;
    uint32_t               uv_pitch = 0;
    size_t                 u_offset = 0;
    size_t                 v_offset = 0;
    std::vector<uint8_t>   bytes{};
    struct wd_frame        gpu_frame{};

    ClientVideoFrameBuffer() {
        wd_frame_init(&gpu_frame);
    }

    ~ClientVideoFrameBuffer() {
        wd_frame_reset(&gpu_frame);
    }

    ClientVideoFrameBuffer(const ClientVideoFrameBuffer&) = delete;
    ClientVideoFrameBuffer& operator=(const ClientVideoFrameBuffer&) = delete;

    ClientVideoFrameBuffer(ClientVideoFrameBuffer&& other) noexcept {
        wd_frame_init(&gpu_frame);
        *this = std::move(other);
    }

    ClientVideoFrameBuffer& operator=(ClientVideoFrameBuffer&& other) noexcept {
        if (this == &other)
        {
            return *this;
        }
        wd_frame_reset(&gpu_frame);
        format    = other.format;
        width     = other.width;
        height    = other.height;
        y_pitch   = other.y_pitch;
        uv_pitch  = other.uv_pitch;
        u_offset  = other.u_offset;
        v_offset  = other.v_offset;
        bytes     = std::move(other.bytes);
        gpu_frame = other.gpu_frame;
        wd_frame_init(&other.gpu_frame);
        other.format = ClientVideoPixelFormat::None;
        other.width = other.height = other.y_pitch = other.uv_pitch = 0;
        other.u_offset = other.v_offset = 0;
        return *this;
    }

    void clear() {
        format   = ClientVideoPixelFormat::None;
        width    = 0;
        height   = 0;
        y_pitch  = 0;
        uv_pitch = 0;
        u_offset = 0;
        v_offset = 0;
        bytes.clear();
        wd_frame_reset(&gpu_frame);
    }

    bool cpu_valid() const {
        if (format != ClientVideoPixelFormat::IYUV || width == 0 || height == 0 ||
            y_pitch < width || uv_pitch < (width + 1u) / 2u)
        {
            return false;
        }
        const size_t y_size    = static_cast<size_t>(y_pitch) * height;
        const size_t uv_height = (height + 1u) / 2u;
        const size_t uv_size   = static_cast<size_t>(uv_pitch) * uv_height;
        return u_offset == y_size && v_offset == y_size + uv_size &&
               v_offset <= bytes.size() && uv_size <= bytes.size() - v_offset;
    }

    bool gpu_valid() const {
        return format == ClientVideoPixelFormat::DRMPrime &&
               wd_frame_valid(&gpu_frame) &&
               gpu_frame.storage == WD_FRAME_STORAGE_DRM_PRIME &&
               gpu_frame.width == width && gpu_frame.height == height;
    }

    bool valid() const {
        return cpu_valid() || gpu_valid();
    }
};

struct ClientDecodedVideoFrame {
    ClientVideoPixelFormat format        = ClientVideoPixelFormat::None;
    uint32_t               width         = 0;
    uint32_t               height        = 0;
    uint64_t               frame_id      = 0;
    uint64_t               content_epoch = 0;
    uint64_t               pts_usec      = 0;
};

bool client_video_decoder_create(ClientVideoDecoder** out_decoder);
void client_video_decoder_destroy(ClientVideoDecoder* decoder);
void client_video_decoder_reset(ClientVideoDecoder* decoder);

bool        client_video_decoder_available(const ClientVideoDecoder* decoder);
uint32_t    client_video_decoder_supported_codecs(const ClientVideoDecoder* decoder);
uint32_t    client_video_decoder_supported_codecs_for_mode(const ClientVideoDecoder* decoder, uint8_t decode_mode);
const char* client_video_decoder_backend_name(const ClientVideoDecoder* decoder);
bool        client_video_decoder_hwdecode_failed_auto(const ClientVideoDecoder* decoder);
uint64_t    client_video_decoder_zero_copy_inputs(const ClientVideoDecoder* decoder);
uint64_t    client_video_decoder_copied_inputs(const ClientVideoDecoder* decoder);
uint64_t    client_video_decoder_gpu_output_frames(const ClientVideoDecoder* decoder);
uint64_t    client_video_decoder_gpu_output_fallbacks(const ClientVideoDecoder* decoder);

bool client_video_decoder_configure(ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config);
bool client_video_decoder_decode(ClientVideoDecoder* decoder, const ClientVideoPacket& packet, ClientDecodedVideoFrame* out_frame);
/* Retrieve another frame produced while processing the most recent packets.
 * The caller must swap each returned frame out before taking the next one. */
bool client_video_decoder_take_frame(ClientVideoDecoder* decoder, ClientDecodedVideoFrame* out_frame);
/* Swap the decoder-owned visible IYUV frame into an application buffer.
 * Call this while excluding concurrent decoder use and immediately after a
 * successful decode. */
bool client_video_decoder_swap_output_frame(ClientVideoDecoder* decoder, ClientVideoFrameBuffer& frame);

} // namespace waydisplay
