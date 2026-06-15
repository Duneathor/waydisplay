#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstdint>

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
const char* client_video_decoder_backend_name(const ClientVideoDecoder* decoder);

bool client_video_decoder_configure(ClientVideoDecoder* decoder, const ClientVideoDecoderConfig& config);
bool client_video_decoder_decode_h265(ClientVideoDecoder* decoder, const ClientVideoPacket& packet,
                                      ClientDecodedVideoFrame* out_frame);

} // namespace waydisplay
