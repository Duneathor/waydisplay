#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace waydisplay {

struct ClientVideoDecoder;

struct ClientVideoDecoderConfig {
    uint8_t  session_id = 0;
    uint64_t connection_token = 0;
    uint64_t content_epoch = 0;
    uint16_t width        = 0;
    uint16_t height       = 0;
    uint16_t coded_width  = 0;
    uint16_t coded_height = 0;
    uint16_t target_fps   = 0;
    uint32_t codec        = 0;
    uint8_t  hwdecode_mode = WD_CLIENT_VIDEO_HWDECODE_AUTO;
};

struct ClientVideoPacket {
    wd_video_frame_payload_header header{};
    const uint8_t*                data = nullptr;
};

enum class ClientVideoPixelFormat : uint8_t {
    None = 0,
    IYUV = 1,
};

struct ClientVideoFrameBuffer {
    ClientVideoPixelFormat format = ClientVideoPixelFormat::None;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t y_pitch = 0;
    uint32_t uv_pitch = 0;
    size_t u_offset = 0;
    size_t v_offset = 0;
    std::vector<uint8_t> bytes{};

    void clear() {
        format = ClientVideoPixelFormat::None;
        width = 0;
        height = 0;
        y_pitch = 0;
        uv_pitch = 0;
        u_offset = 0;
        v_offset = 0;
        bytes.clear();
    }

    bool valid() const {
        if (format != ClientVideoPixelFormat::IYUV || width == 0 || height == 0 ||
            y_pitch < width || uv_pitch < (width + 1u) / 2u)
        {
            return false;
        }
        const size_t y_size = static_cast<size_t>(y_pitch) * height;
        const size_t uv_height = (height + 1u) / 2u;
        const size_t uv_size = static_cast<size_t>(uv_pitch) * uv_height;
        return u_offset == y_size && v_offset == y_size + uv_size &&
               v_offset <= bytes.size() && uv_size <= bytes.size() - v_offset;
    }
};

struct ClientDecodedVideoFrame {
    ClientVideoPixelFormat format = ClientVideoPixelFormat::None;
    uint32_t        width         = 0;
    uint32_t        height        = 0;
    uint64_t        frame_id      = 0;
    uint64_t        content_epoch = 0;
    uint64_t        pts_usec      = 0;
};

bool client_video_decoder_create(ClientVideoDecoder** out_decoder);
void client_video_decoder_destroy(ClientVideoDecoder* decoder);
void client_video_decoder_reset(ClientVideoDecoder* decoder);

bool        client_video_decoder_available(const ClientVideoDecoder* decoder);
uint32_t    client_video_decoder_supported_codecs(const ClientVideoDecoder* decoder);
const char* client_video_decoder_backend_name(const ClientVideoDecoder* decoder);
bool        client_video_decoder_hwdecode_failed_auto(const ClientVideoDecoder* decoder);

bool client_video_decoder_configure(ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config);
bool client_video_decoder_decode(ClientVideoDecoder* decoder, const ClientVideoPacket& packet,
                                 ClientDecodedVideoFrame* out_frame);
/* Swap the decoder-owned visible IYUV frame into an application buffer.
 * Call this while excluding concurrent decoder use and immediately after a
 * successful decode. */
bool client_video_decoder_swap_output_frame(ClientVideoDecoder* decoder, ClientVideoFrameBuffer& frame);

} // namespace waydisplay
