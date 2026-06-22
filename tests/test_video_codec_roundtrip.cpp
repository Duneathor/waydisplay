#include "video_decoder.hpp"
#include "wd_video_encoder.h"

#include "waydisplay/wd_protocol.h"

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

#define CHECK(condition)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);   \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

constexpr uint32_t kWidth = 65;
constexpr uint32_t kHeight = 49;
constexpr uint32_t kCodedWidth = 66;
constexpr uint32_t kCodedHeight = 50;
constexpr uint32_t kStride = 72;

void fill_frame(std::vector<uint32_t>& pixels, uint32_t frame_number) {
    std::fill(pixels.begin(), pixels.end(), UINT32_C(0xff000000));
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t phase = frame_number * 29u;
            const uint32_t red = (x * 4u + phase) & 0xffu;
            const uint32_t green = (y * 6u + phase * 2u) & 0xffu;
            const uint32_t blue = ((x + y) * 3u + phase * 3u) & 0xffu;
            pixels[static_cast<size_t>(y) * kStride + x] =
                UINT32_C(0xff000000) | (red << 16u) | (green << 8u) | blue;
        }
    }
}

uint64_t luma_checksum(const ClientVideoFrameBuffer& frame) {
    const size_t y_size = static_cast<size_t>(frame.y_pitch) * frame.height;
    return std::accumulate(frame.bytes.begin(), frame.bytes.begin() + static_cast<ptrdiff_t>(y_size),
                           UINT64_C(0));
}

bool configure_pair(wd_video_encoder* encoder, ClientVideoDecoder* decoder, uint32_t codec,
                    uint64_t content_epoch) {
    wd_video_encoder_config encoder_config{};
    encoder_config.session_id = 11;
    encoder_config.connection_token = UINT64_C(0x1122334455667788);
    encoder_config.content_epoch = content_epoch;
    encoder_config.width = kWidth;
    encoder_config.height = kHeight;
    encoder_config.target_fps = 30;
    encoder_config.bitrate_kib_per_second = 4096;
    encoder_config.codec = codec;
    CHECK(wd_video_encoder_configure(encoder, &encoder_config));

    ClientVideoDecoderConfig decoder_config{};
    decoder_config.session_id = encoder_config.session_id;
    decoder_config.connection_token = encoder_config.connection_token;
    decoder_config.content_epoch = content_epoch;
    decoder_config.width = kWidth;
    decoder_config.height = kHeight;
    decoder_config.coded_width = kCodedWidth;
    decoder_config.coded_height = kCodedHeight;
    decoder_config.target_fps = encoder_config.target_fps;
    decoder_config.codec = codec;
    decoder_config.hwdecode_mode = WD_CLIENT_VIDEO_HWDECODE_OFF;
    CHECK(waydisplay::client_video_decoder_configure(decoder, decoder_config));
    return true;
}

bool run_codec(uint32_t codec) {
    wd_video_encoder* encoder = nullptr;
    ClientVideoDecoder* decoder = nullptr;
    CHECK(wd_video_encoder_create(&encoder, "software", nullptr));
    CHECK(waydisplay::client_video_decoder_create(&decoder));
    CHECK(configure_pair(encoder, decoder, codec, 1));

    std::vector<uint32_t> pixels(static_cast<size_t>(kStride) * kHeight);
    std::vector<uint64_t> checksums;
    std::vector<wd_video_frame_payload_header> submitted_headers;
    uint64_t previous_frame_id = 0;
    bool saw_keyframe = false;

    for (uint32_t frame_number = 0; frame_number < 24 && checksums.size() < 3; ++frame_number)
    {
        fill_frame(pixels, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels = pixels.data();
        input.width = kWidth;
        input.height = kHeight;
        input.stride_pixels = kStride;
        input.pts_usec = UINT64_C(2000000) + static_cast<uint64_t>(frame_number) * UINT64_C(33333);

        wd_video_encoder_packet encoded{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &encoded));
        if (encoded.header.data_size == 0)
        {
            continue;
        }

        CHECK(encoded.header.frame_id > previous_frame_id);
        CHECK(encoded.header.width == kWidth);
        CHECK(encoded.header.height == kHeight);
        CHECK(encoded.header.coded_width == kCodedWidth);
        CHECK(encoded.header.coded_height == kCodedHeight);
        CHECK(wd_video_frame_payload_size_is_valid(
            &encoded.header,
            static_cast<uint32_t>(sizeof(encoded.header)) + encoded.header.data_size));
        previous_frame_id = encoded.header.frame_id;
        saw_keyframe = saw_keyframe || (encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0;

        ClientVideoPacket packet{};
        packet.header = encoded.header;
        packet.data = encoded.data;
        submitted_headers.push_back(encoded.header);
        ClientDecodedVideoFrame decoded{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &decoded));

        for (;;)
        {
            if (decoded.format == ClientVideoPixelFormat::None)
            {
                break;
            }

            const auto submitted = std::find_if(
                submitted_headers.begin(), submitted_headers.end(),
                [&decoded](const wd_video_frame_payload_header& header) {
                    return header.frame_id == decoded.frame_id;
                });
            CHECK(submitted != submitted_headers.end());

            ClientVideoFrameBuffer output{};
            CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
            CHECK(output.valid());
            CHECK(decoded.format == ClientVideoPixelFormat::IYUV);
            CHECK(decoded.width == kWidth);
            CHECK(decoded.height == kHeight);
            CHECK(decoded.content_epoch == submitted->content_epoch);
            CHECK(decoded.pts_usec == submitted->pts_usec);
            submitted_headers.erase(submitted);
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
    CHECK(std::adjacent_find(checksums.begin(), checksums.end(), std::not_equal_to<>()) !=
          checksums.end());

    waydisplay::client_video_decoder_reset(decoder);
    wd_video_encoder_reset(encoder);
    CHECK(configure_pair(encoder, decoder, codec, 2));
    CHECK(wd_video_encoder_request_keyframe(encoder));

    bool decoded_new_epoch = false;
    bool saw_new_epoch_keyframe = false;
    submitted_headers.clear();
    for (uint32_t frame_number = 30; frame_number < 42 && !decoded_new_epoch; ++frame_number)
    {
        fill_frame(pixels, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels = pixels.data();
        input.width = kWidth;
        input.height = kHeight;
        input.stride_pixels = kStride;
        input.pts_usec = UINT64_C(5000000) + static_cast<uint64_t>(frame_number) * UINT64_C(33333);
        wd_video_encoder_packet encoded{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &encoded));
        if (encoded.header.data_size == 0)
        {
            continue;
        }
        CHECK(encoded.header.content_epoch == 2);
        if (!saw_new_epoch_keyframe)
        {
            CHECK(encoded.header.frame_id == 1);
            CHECK((encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
            saw_new_epoch_keyframe = true;
        }
        submitted_headers.push_back(encoded.header);

        ClientVideoPacket packet{encoded.header, encoded.data};
        ClientDecodedVideoFrame decoded{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &decoded));
        for (;;)
        {
            if (decoded.format == ClientVideoPixelFormat::None)
            {
                break;
            }
            const auto submitted = std::find_if(
                submitted_headers.begin(), submitted_headers.end(),
                [&decoded](const wd_video_frame_payload_header& header) {
                    return header.frame_id == decoded.frame_id;
                });
            CHECK(submitted != submitted_headers.end());
            ClientVideoFrameBuffer output{};
            CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
            CHECK(output.valid());
            CHECK(decoded.content_epoch == 2);
            CHECK(decoded.pts_usec == submitted->pts_usec);
            submitted_headers.erase(submitted);
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

} // namespace

int main() {
    wd_video_encoder* encoder = nullptr;
    ClientVideoDecoder* decoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "software", nullptr) ||
        !waydisplay::client_video_decoder_create(&decoder))
    {
        wd_video_encoder_destroy(encoder);
        waydisplay::client_video_decoder_destroy(decoder);
        return 1;
    }
    const uint32_t common = wd_video_encoder_supported_codecs(encoder) &
                            waydisplay::client_video_decoder_supported_codecs(decoder);
    wd_video_encoder_destroy(encoder);
    waydisplay::client_video_decoder_destroy(decoder);

    if (common == 0)
    {
        std::fprintf(stderr, "SKIP: no codec is available to both encoder and decoder\n");
        return 77;
    }
    if ((common & WD_VIDEO_CODEC_H264) != 0 && !run_codec(WD_VIDEO_CODEC_H264))
    {
        return 1;
    }
    if ((common & WD_VIDEO_CODEC_H265) != 0 && !run_codec(WD_VIDEO_CODEC_H265))
    {
        return 1;
    }
    return 0;
}
