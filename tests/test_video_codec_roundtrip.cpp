#include "video_keyframe_recovery.h"
#include "video_decoder.hpp"
#include "waydisplay/wd_protocol.h"
#include "wd_video_encoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numeric>
#include <vector>

namespace {

using waydisplay::ClientDecodedVideoFrame;
using waydisplay::ClientVideoDecoder;
using waydisplay::ClientVideoDecoderConfig;
using waydisplay::ClientVideoFrameBuffer;
using waydisplay::ClientVideoPacket;
using waydisplay::ClientVideoPixelFormat;

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                                             \
            return false;                                                                                                                  \
        }                                                                                                                                  \
    } while (false)

constexpr uint32_t kWidth        = 65;
constexpr uint32_t kHeight       = 49;
constexpr uint32_t kResizeWidth  = 67;
constexpr uint32_t kResizeHeight = 51;
constexpr uint32_t kStride       = 72;
constexpr uint64_t kFrameStepUsec = UINT64_C(33333);

uint32_t coded_dimension(uint32_t value) {
    return (value + 1u) & ~UINT32_C(1);
}

uint32_t expected_bright_quadrant(uint32_t frame_number) {
    return frame_number & 3u;
}

void fill_frame(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t frame_number) {
    std::fill(pixels.begin(), pixels.end(), UINT32_C(0xff101010));
    const uint32_t bright_quadrant = expected_bright_quadrant(frame_number);
    const uint32_t split_x = width / 2u;
    const uint32_t split_y = height / 2u;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t quadrant = (x >= split_x ? 1u : 0u) | (y >= split_y ? 2u : 0u);
            const uint32_t base = quadrant == bright_quadrant ? 0xe0u : 0x18u;
            const uint32_t detail = (x * 3u + y * 5u + frame_number * 7u) & 0x0fu;
            const uint32_t level = std::min<uint32_t>(255u, base + detail);
            pixels[static_cast<size_t>(y) * kStride + x] =
                UINT32_C(0xff000000) | (level << 16u) | (level << 8u) | level;
        }
    }
}

uint64_t luma_checksum(const ClientVideoFrameBuffer& frame) {
    const size_t y_size = static_cast<size_t>(frame.y_pitch) * frame.height;
    return std::accumulate(frame.bytes.begin(), frame.bytes.begin() + static_cast<ptrdiff_t>(y_size), UINT64_C(0));
}

uint32_t brightest_luma_quadrant(const ClientVideoFrameBuffer& frame) {
    uint64_t sums[4]{};
    uint64_t counts[4]{};
    const uint32_t split_x = frame.width / 2u;
    const uint32_t split_y = frame.height / 2u;
    for (uint32_t y = 0; y < frame.height; ++y)
    {
        for (uint32_t x = 0; x < frame.width; ++x)
        {
            const uint32_t quadrant = (x >= split_x ? 1u : 0u) | (y >= split_y ? 2u : 0u);
            sums[quadrant] += frame.bytes[static_cast<size_t>(y) * frame.y_pitch + x];
            counts[quadrant]++;
        }
    }

    uint32_t brightest = 0;
    for (uint32_t quadrant = 1; quadrant < 4; ++quadrant)
    {
        if (sums[quadrant] * counts[brightest] > sums[brightest] * counts[quadrant])
        {
            brightest = quadrant;
        }
    }
    return brightest;
}

struct SubmittedFrameExpectation {
    wd_video_frame_payload_header header{};
    uint32_t                      bright_quadrant = 0;
};

bool expected_quadrant_for_pts(uint64_t pts_usec, uint64_t base_pts_usec, uint32_t* quadrant) {
    if (!quadrant || pts_usec < base_pts_usec)
    {
        return false;
    }
    const uint64_t delta = pts_usec - base_pts_usec;
    if (delta % kFrameStepUsec != 0)
    {
        return false;
    }
    *quadrant = expected_bright_quadrant(static_cast<uint32_t>(delta / kFrameStepUsec));
    return true;
}

bool configure_pair(wd_video_encoder* encoder, ClientVideoDecoder* decoder, uint32_t codec, uint64_t content_epoch,
                    uint32_t width = kWidth, uint32_t height = kHeight) {
    wd_video_encoder_config encoder_config{};
    encoder_config.session_id             = 11;
    encoder_config.connection_token       = UINT64_C(0x1122334455667788);
    encoder_config.content_epoch          = content_epoch;
    encoder_config.width                  = static_cast<uint16_t>(width);
    encoder_config.height                 = static_cast<uint16_t>(height);
    encoder_config.target_fps             = 30;
    encoder_config.bitrate_kib_per_second = 4096;
    encoder_config.codec                  = codec;
    CHECK(wd_video_encoder_configure(encoder, &encoder_config));

    ClientVideoDecoderConfig decoder_config{};
    decoder_config.session_id       = encoder_config.session_id;
    decoder_config.connection_token = encoder_config.connection_token;
    decoder_config.content_epoch    = content_epoch;
    decoder_config.width            = static_cast<uint16_t>(width);
    decoder_config.height           = static_cast<uint16_t>(height);
    decoder_config.coded_width      = static_cast<uint16_t>(coded_dimension(width));
    decoder_config.coded_height     = static_cast<uint16_t>(coded_dimension(height));
    decoder_config.target_fps       = encoder_config.target_fps;
    decoder_config.codec            = codec;
    decoder_config.decode_mode     = WD_CLIENT_VIDEO_DECODER_SOFTWARE;
    CHECK(waydisplay::client_video_decoder_configure(decoder, decoder_config));
    return true;
}

bool run_codec(uint32_t codec) {
    wd_video_encoder*   encoder = nullptr;
    ClientVideoDecoder* decoder = nullptr;
    CHECK(wd_video_encoder_create(&encoder, "software"));
    CHECK(waydisplay::client_video_decoder_create(&decoder));
    CHECK(configure_pair(encoder, decoder, codec, 1));

    std::vector<uint32_t>                 pixels(static_cast<size_t>(kStride) * kResizeHeight);
    std::vector<uint64_t>                 checksums;
    std::vector<SubmittedFrameExpectation> submitted_frames;
    uint64_t                                   previous_frame_id = 0;
    bool                                       saw_keyframe      = false;

    for (uint32_t frame_number = 0; frame_number < 24 && checksums.size() < 3; ++frame_number)
    {
        fill_frame(pixels, kWidth, kHeight, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels        = pixels.data();
        input.width         = kWidth;
        input.height        = kHeight;
        input.stride_pixels = kStride;
        input.pts_usec      = UINT64_C(2000000) + static_cast<uint64_t>(frame_number) * kFrameStepUsec;

        wd_video_encoder_packet encoded{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &encoded));
        if (encoded.header.data_size == 0)
        {
            continue;
        }

        CHECK(encoded.header.frame_id > previous_frame_id);
        CHECK(encoded.header.width == kWidth);
        CHECK(encoded.header.height == kHeight);
        CHECK(encoded.header.coded_width == coded_dimension(kWidth));
        CHECK(encoded.header.coded_height == coded_dimension(kHeight));
        CHECK(wd_video_frame_payload_size_is_valid(&encoded.header,
                                                   static_cast<uint32_t>(sizeof(encoded.header)) + encoded.header.data_size));
        previous_frame_id = encoded.header.frame_id;
        if ((encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0)
        {
            CHECK(wd_client_video_keyframe_validate(codec, encoded.data, encoded.header.data_size) == WD_CLIENT_VIDEO_KEYFRAME_VALID);
        }
        saw_keyframe = saw_keyframe || (encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0;

        ClientVideoPacket packet{};
        packet.header = encoded.header;
        packet.data   = encoded.data;
        uint32_t expected_quadrant = 0;
        CHECK(expected_quadrant_for_pts(encoded.header.pts_usec, UINT64_C(2000000), &expected_quadrant));
        submitted_frames.push_back({encoded.header, expected_quadrant});
        ClientDecodedVideoFrame decoded{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &decoded));

        for (;;)
        {
            if (decoded.format == ClientVideoPixelFormat::None)
            {
                break;
            }

            const auto submitted =
                std::find_if(submitted_frames.begin(), submitted_frames.end(),
                             [&decoded](const SubmittedFrameExpectation& candidate) {
                                 return candidate.header.frame_id == decoded.frame_id;
                             });
            CHECK(submitted != submitted_frames.end());

            ClientVideoFrameBuffer output{};
            CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
            CHECK(output.valid());
            CHECK(decoded.format == ClientVideoPixelFormat::IYUV);
            CHECK(decoded.width == kWidth);
            CHECK(decoded.height == kHeight);
            CHECK(decoded.content_epoch == submitted->header.content_epoch);
            CHECK(decoded.pts_usec == submitted->header.pts_usec);
            CHECK(brightest_luma_quadrant(output) == submitted->bright_quadrant);
            submitted_frames.erase(submitted);
            checksums.push_back(luma_checksum(output));

            decoded = ClientDecodedVideoFrame{};
            if (!waydisplay::client_video_decoder_take_frame(decoder, &decoded))
            {
                break;
            }
        }
    }

    CHECK(saw_keyframe);
    CHECK(checksums.size() >= 2);
    CHECK(std::adjacent_find(checksums.begin(), checksums.end(), std::not_equal_to<>()) != checksums.end());

    waydisplay::client_video_decoder_reset(decoder);
    wd_video_encoder_reset(encoder);
    CHECK(configure_pair(encoder, decoder, codec, 2, kResizeWidth, kResizeHeight));
    CHECK(wd_video_encoder_request_keyframe(encoder));

    bool decoded_new_epoch      = false;
    bool saw_new_epoch_keyframe = false;
    submitted_frames.clear();
    for (uint32_t frame_number = 30; frame_number < 42 && !decoded_new_epoch; ++frame_number)
    {
        fill_frame(pixels, kResizeWidth, kResizeHeight, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels        = pixels.data();
        input.width         = kResizeWidth;
        input.height        = kResizeHeight;
        input.stride_pixels = kStride;
        input.pts_usec      = UINT64_C(5000000) + static_cast<uint64_t>(frame_number) * kFrameStepUsec;
        wd_video_encoder_packet encoded{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &encoded));
        if (encoded.header.data_size == 0)
        {
            continue;
        }
        CHECK(encoded.header.content_epoch == 2);
        CHECK(encoded.header.width == kResizeWidth && encoded.header.height == kResizeHeight);
        CHECK(encoded.header.coded_width == coded_dimension(kResizeWidth) &&
              encoded.header.coded_height == coded_dimension(kResizeHeight));
        if (!saw_new_epoch_keyframe)
        {
            CHECK(encoded.header.frame_id == 1);
            CHECK((encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
            CHECK(wd_client_video_keyframe_validate(codec, encoded.data, encoded.header.data_size) == WD_CLIENT_VIDEO_KEYFRAME_VALID);
            saw_new_epoch_keyframe = true;
        }
        uint32_t expected_quadrant = 0;
        CHECK(expected_quadrant_for_pts(encoded.header.pts_usec, UINT64_C(5000000), &expected_quadrant));
        submitted_frames.push_back({encoded.header, expected_quadrant});

        ClientVideoPacket       packet{encoded.header, encoded.data};
        ClientDecodedVideoFrame decoded{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &decoded));
        for (;;)
        {
            if (decoded.format == ClientVideoPixelFormat::None)
            {
                break;
            }
            const auto submitted =
                std::find_if(submitted_frames.begin(), submitted_frames.end(),
                             [&decoded](const SubmittedFrameExpectation& candidate) {
                                 return candidate.header.frame_id == decoded.frame_id;
                             });
            CHECK(submitted != submitted_frames.end());
            ClientVideoFrameBuffer output{};
            CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
            CHECK(output.valid());
            CHECK(output.width == kResizeWidth && output.height == kResizeHeight);
            CHECK(decoded.content_epoch == 2);
            CHECK(decoded.pts_usec == submitted->header.pts_usec);
            CHECK(brightest_luma_quadrant(output) == submitted->bright_quadrant);
            submitted_frames.erase(submitted);
            decoded_new_epoch = true;

            decoded = ClientDecodedVideoFrame{};
            if (!waydisplay::client_video_decoder_take_frame(decoder, &decoded))
            {
                break;
            }
        }
    }
    CHECK(saw_new_epoch_keyframe);
    CHECK(decoded_new_epoch);

    waydisplay::client_video_decoder_destroy(decoder);
    wd_video_encoder_destroy(encoder);
    return true;
}

/* The server can discard a packet after it has been encoded if another TCP
 * video send is pending. The encoder is then instructed to produce a fresh
 * random-access picture before the client resumes receiving dependent frames. */
bool run_dropped_reference_recovery(uint32_t codec) {
    wd_video_encoder* encoder = nullptr;
    ClientVideoDecoder* decoder = nullptr;
    CHECK(wd_video_encoder_create(&encoder, "software"));
    CHECK(waydisplay::client_video_decoder_create(&decoder));
    CHECK(configure_pair(encoder, decoder, codec, 7));

    std::vector<uint32_t> pixels(static_cast<size_t>(kStride) * kHeight);
    bool saw_first_keyframe = false;
    bool dropped_reference = false;
    bool resumed_at_keyframe = false;
    uint32_t frame_number = 0;
    for (; frame_number < 64 && !dropped_reference; ++frame_number)
    {
        fill_frame(pixels, kWidth, kHeight, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels = pixels.data();
        input.width = kWidth;
        input.height = kHeight;
        input.stride_pixels = kStride;
        input.pts_usec = UINT64_C(6000000) + static_cast<uint64_t>(frame_number) * kFrameStepUsec;
        wd_video_encoder_packet packet{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &packet));
        if (packet.header.data_size == 0)
        {
            continue;
        }
        if ((packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0)
        {
            saw_first_keyframe = true;
            continue;
        }
        if (saw_first_keyframe)
        {
            /* Packet is intentionally not passed to the decoder. */
            dropped_reference = true;
        }
    }
    CHECK(saw_first_keyframe);
    CHECK(dropped_reference);
    CHECK(wd_video_encoder_request_keyframe(encoder));
    waydisplay::client_video_decoder_reset(decoder);
    CHECK(configure_pair(encoder, decoder, codec, 7));

    for (; frame_number < 96 && !resumed_at_keyframe; ++frame_number)
    {
        fill_frame(pixels, kWidth, kHeight, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels = pixels.data();
        input.width = kWidth;
        input.height = kHeight;
        input.stride_pixels = kStride;
        input.pts_usec = UINT64_C(6000000) + static_cast<uint64_t>(frame_number) * kFrameStepUsec;
        wd_video_encoder_packet packet{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &packet));
        if (packet.header.data_size == 0 || (packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) == 0)
        {
            continue;
        }
        CHECK(wd_client_video_keyframe_validate(codec, packet.data, packet.header.data_size) == WD_CLIENT_VIDEO_KEYFRAME_VALID);
        ClientVideoPacket received{packet.header, packet.data};
        ClientDecodedVideoFrame decoded{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, received, &decoded));
        resumed_at_keyframe = true;
    }
    CHECK(resumed_at_keyframe);
    waydisplay::client_video_decoder_destroy(decoder);
    wd_video_encoder_destroy(encoder);
    return true;
}

} // namespace

int main() {
    wd_video_encoder*   encoder = nullptr;
    ClientVideoDecoder* decoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "software") || !waydisplay::client_video_decoder_create(&decoder))
    {
        wd_video_encoder_destroy(encoder);
        waydisplay::client_video_decoder_destroy(decoder);
        return 1;
    }
    const uint32_t common = wd_video_encoder_supported_codecs(encoder) & waydisplay::client_video_decoder_supported_codecs(decoder);
    wd_video_encoder_destroy(encoder);
    waydisplay::client_video_decoder_destroy(decoder);

    if (common == 0)
    {
        std::fprintf(stderr, "SKIP: no codec is available to both encoder and decoder\n");
        return 77;
    }
    if ((common & WD_VIDEO_CODEC_H264) != 0 && !run_dropped_reference_recovery(WD_VIDEO_CODEC_H264))
    {
        return 1;
    }
    if ((common & WD_VIDEO_CODEC_H265) != 0 && !run_dropped_reference_recovery(WD_VIDEO_CODEC_H265))
    {
        return 1;
    }
    if ((common & WD_VIDEO_CODEC_AV1) != 0 && !run_dropped_reference_recovery(WD_VIDEO_CODEC_AV1))
    {
        return 1;
    }
    if ((common & WD_VIDEO_CODEC_H264) != 0 && !run_codec(WD_VIDEO_CODEC_H264))
    {
        return 1;
    }
    if ((common & WD_VIDEO_CODEC_H265) != 0 && !run_codec(WD_VIDEO_CODEC_H265))
    {
        return 1;
    }
    if ((common & WD_VIDEO_CODEC_AV1) != 0 && !run_codec(WD_VIDEO_CODEC_AV1))
    {
        return 1;
    }
    return 0;
}
