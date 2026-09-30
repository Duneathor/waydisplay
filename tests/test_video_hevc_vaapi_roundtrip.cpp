#include "video_decoder.hpp"
#include "waydisplay/wd_protocol.h"
#include "wd_video_encoder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using waydisplay::ClientDecodedVideoFrame;
using waydisplay::ClientVideoDecoder;
using waydisplay::ClientVideoDecoderConfig;
using waydisplay::ClientVideoFrameBuffer;
using waydisplay::ClientVideoPacket;
using waydisplay::ClientVideoPixelFormat;

#define CHECK(condition)                                                                 \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                     \
            return false;                                                                \
        }                                                                                \
    } while (false)

constexpr uint32_t kWidth  = 256;
constexpr uint32_t kHeight = 256;
constexpr uint64_t kToken  = UINT64_C(0x4845564356414150);

void fill_pattern(std::vector<uint32_t>& pixels, uint32_t frame_number) {
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t band = ((x / 32u) + (y / 32u) + frame_number) & 7u;
            const uint32_t red = (band * 29u + frame_number * 7u) & 0xffu;
            const uint32_t green = (x + frame_number * 13u) & 0xffu;
            const uint32_t blue = (y + frame_number * 17u) & 0xffu;
            pixels[static_cast<size_t>(y) * kWidth + x] =
                UINT32_C(0xff000000) | (red << 16u) | (green << 8u) | blue;
        }
    }
}

struct DecodedContentStats {
    uint32_t decoded_frames = 0;
    uint32_t spatially_varied_frames = 0;
    uint32_t signature_changes = 0;
    uint64_t previous_signature = 0;
    bool     have_signature = false;
};

uint64_t sampled_luma_signature(const ClientVideoFrameBuffer& frame,
                                uint8_t& min_luma, uint8_t& max_luma) {
    uint64_t hash = UINT64_C(1469598103934665603);
    min_luma = UINT8_MAX;
    max_luma = 0;
    for (uint32_t y = 0; y < frame.height; y += 8u)
    {
        const uint8_t* row = frame.bytes.data() + static_cast<size_t>(y) * frame.y_pitch;
        for (uint32_t x = 0; x < frame.width; x += 8u)
        {
            const uint8_t sample = row[x];
            min_luma = std::min(min_luma, sample);
            max_luma = std::max(max_luma, sample);
            hash ^= sample;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

bool drain_decoded(ClientVideoDecoder* decoder, ClientDecodedVideoFrame decoded,
                   DecodedContentStats& stats) {
    for (;;)
    {
        if (decoded.format == ClientVideoPixelFormat::None)
        {
            return true;
        }

        CHECK(decoded.format == ClientVideoPixelFormat::IYUV);
        CHECK(decoded.width == kWidth);
        CHECK(decoded.height == kHeight);

        ClientVideoFrameBuffer output{};
        CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
        CHECK(output.valid());
        CHECK(output.width == kWidth);
        CHECK(output.height == kHeight);
        CHECK(output.cpu_valid());

        uint8_t min_luma = 0;
        uint8_t max_luma = 0;
        const uint64_t signature = sampled_luma_signature(output, min_luma, max_luma);
        if (static_cast<uint32_t>(max_luma) - static_cast<uint32_t>(min_luma) >= 12u)
        {
            stats.spatially_varied_frames++;
        }
        if (stats.have_signature && signature != stats.previous_signature)
        {
            stats.signature_changes++;
        }
        stats.previous_signature = signature;
        stats.have_signature = true;
        stats.decoded_frames++;

        decoded = ClientDecodedVideoFrame{};
        if (!waydisplay::client_video_decoder_take_frame(decoder, &decoded))
        {
            return true;
        }
    }
}

bool run_hevc_roundtrip() {
    wd_video_encoder* encoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "vaapi"))
    {
        return false;
    }

    if ((wd_video_encoder_supported_codecs(encoder) & WD_VIDEO_CODEC_H265) == 0)
    {
        wd_video_encoder_destroy(encoder);
        return false;
    }

    ClientVideoDecoder* decoder = nullptr;
    CHECK(waydisplay::client_video_decoder_create(&decoder));
    if ((waydisplay::client_video_decoder_supported_codecs(decoder) & WD_VIDEO_CODEC_H265) == 0)
    {
        waydisplay::client_video_decoder_destroy(decoder);
        wd_video_encoder_destroy(encoder);
        return false;
    }

    wd_video_encoder_config encoder_config{};
    encoder_config.session_id             = 31;
    encoder_config.connection_token       = kToken;
    encoder_config.content_epoch          = 7;
    encoder_config.width                  = kWidth;
    encoder_config.height                 = kHeight;
    encoder_config.target_fps             = 30;
    encoder_config.bitrate_kib_per_second = 8192;
    encoder_config.codec                  = WD_VIDEO_CODEC_H265;
    CHECK(wd_video_encoder_configure(encoder, &encoder_config));
    CHECK(wd_video_encoder_request_keyframe(encoder));

    ClientVideoDecoderConfig decoder_config{};
    decoder_config.session_id       = encoder_config.session_id;
    decoder_config.connection_token = encoder_config.connection_token;
    decoder_config.content_epoch    = encoder_config.content_epoch;
    decoder_config.width            = kWidth;
    decoder_config.height           = kHeight;
    decoder_config.coded_width      = kWidth;
    decoder_config.coded_height     = kHeight;
    decoder_config.target_fps       = encoder_config.target_fps;
    decoder_config.codec            = WD_VIDEO_CODEC_H265;
    decoder_config.decode_mode      = WD_CLIENT_VIDEO_DECODER_SOFTWARE;
    CHECK(waydisplay::client_video_decoder_configure(decoder, decoder_config));

    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
    uint32_t packets_submitted = 0;
    DecodedContentStats decoded_stats{};
    bool saw_keyframe = false;
    bool saw_interframe = false;

    for (uint32_t frame_number = 0; frame_number < 32; ++frame_number)
    {
        fill_pattern(pixels, frame_number);

        wd_video_encoder_input_xrgb8888 input{};
        input.pixels        = pixels.data();
        input.width         = kWidth;
        input.height        = kHeight;
        input.stride_pixels = kWidth;
        input.pts_usec =
            UINT64_C(1000000) + static_cast<uint64_t>(frame_number) * UINT64_C(33333);

        wd_video_encoder_packet encoded{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &encoded));
        if (encoded.header.data_size == 0)
        {
            continue;
        }

        CHECK(encoded.buffer != nullptr);
        CHECK(encoded.data != nullptr);
        CHECK(encoded.header.codec == WD_VIDEO_CODEC_H265);
        CHECK(encoded.header.data_size >= 4);
        CHECK(encoded.data[0] == 0 && encoded.data[1] == 0 &&
              (encoded.data[2] == 1 ||
               (encoded.data[2] == 0 && encoded.data[3] == 1)));

        saw_keyframe =
            saw_keyframe || (encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0;
        saw_interframe =
            saw_interframe || (encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) == 0;

        ClientVideoPacket packet{};
        packet.header = encoded.header;
        packet.data   = encoded.data;

        ClientDecodedVideoFrame decoded{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &decoded));
        packets_submitted++;
        wd_video_encoder_packet_release(&encoded);

        CHECK(drain_decoded(decoder, decoded, decoded_stats));
    }

    CHECK(packets_submitted >= 2);
    CHECK(saw_keyframe);
    CHECK(saw_interframe);
    CHECK(decoded_stats.decoded_frames >= 2);
    CHECK(decoded_stats.spatially_varied_frames == decoded_stats.decoded_frames);
    CHECK(decoded_stats.signature_changes > 0);

    waydisplay::client_video_decoder_destroy(decoder);
    wd_video_encoder_destroy(encoder);
    return true;
}

} // namespace

int main() {
    wd_video_encoder* probe_encoder = nullptr;
    ClientVideoDecoder* probe_decoder = nullptr;
    if (!wd_video_encoder_create(&probe_encoder, "vaapi") ||
        !waydisplay::client_video_decoder_create(&probe_decoder))
    {
        wd_video_encoder_destroy(probe_encoder);
        waydisplay::client_video_decoder_destroy(probe_decoder);
        return 77;
    }

    const bool supported =
        (wd_video_encoder_supported_codecs(probe_encoder) & WD_VIDEO_CODEC_H265) != 0 &&
        (waydisplay::client_video_decoder_supported_codecs(probe_decoder) &
         WD_VIDEO_CODEC_H265) != 0;
    wd_video_encoder_destroy(probe_encoder);
    waydisplay::client_video_decoder_destroy(probe_decoder);
    if (!supported)
    {
        std::fprintf(stderr, "SKIP: VAAPI HEVC encode/software HEVC decode unavailable\n");
        return 77;
    }

    return run_hevc_roundtrip() ? 0 : 1;
}
