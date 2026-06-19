#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstdint>
#include <vector>

namespace waydisplay {

struct ClientVideoDecoder;

struct ClientVideoDecoderConfig {
    uint8_t  session_id = 0;
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

struct ClientDecodedVideoFrame {
    const uint32_t* pixels        = nullptr;
    uint32_t        width         = 0;
    uint32_t        height        = 0;
    uint32_t        stride_pixels = 0;
    uint64_t        frame_id      = 0;
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
/* Swap the decoder-owned visible BGRA frame into an application buffer.
 * Call this while excluding concurrent decoder use and immediately after a
 * successful decode whose output frame points at the decoder buffer. */
bool client_video_decoder_swap_output_pixels(ClientVideoDecoder* decoder, std::vector<uint32_t>& pixels);

} // namespace waydisplay
