#include "waydisplay/wd_protocol.h"
#include "wd_video_encoder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef WAYDISPLAY_TEST_HAVE_H264_ENCODER
#define WAYDISPLAY_TEST_HAVE_H264_ENCODER 0
#endif
#ifndef WAYDISPLAY_TEST_HAVE_H265_ENCODER
#define WAYDISPLAY_TEST_HAVE_H265_ENCODER 0
#endif

namespace {

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                                             \
            return false;                                                                                                                  \
        }                                                                                                                                  \
    } while (false)

constexpr uint32_t kWidth           = 65;
constexpr uint32_t kHeight          = 49;
constexpr uint32_t kStride          = 71;
constexpr uint64_t kConnectionToken = UINT64_C(0x1020304050607080);

uint32_t compiled_codec_mask() {
    uint32_t mask = 0;
#if WAYDISPLAY_TEST_HAVE_H264_ENCODER
    mask |= WD_VIDEO_CODEC_H264;
#endif
#if WAYDISPLAY_TEST_HAVE_H265_ENCODER
    mask |= WD_VIDEO_CODEC_H265;
#endif
    return mask;
}

void fill_pattern(std::vector<uint32_t>& pixels, uint32_t frame_index) {
    std::fill(pixels.begin(), pixels.end(), UINT32_C(0xff101010));
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t red                           = (x * 3u + frame_index * 17u) & 0xffu;
            const uint32_t green                         = (y * 5u + frame_index * 11u) & 0xffu;
            const uint32_t blue                          = ((x ^ y) * 7u + frame_index * 23u) & 0xffu;
            pixels[static_cast<size_t>(y) * kStride + x] = UINT32_C(0xff000000) | (red << 16u) | (green << 8u) | blue;
        }
    }
}

bool packet_metadata_is_valid(const wd_video_encoder_packet& packet, const wd_video_encoder_config& config) {
    CHECK(packet.data != nullptr);
    CHECK(packet.header.data_size != 0);
    CHECK(wd_video_frame_payload_size_is_valid(&packet.header, static_cast<uint32_t>(sizeof(packet.header)) + packet.header.data_size));
    CHECK(packet.header.session_id == config.session_id);
    CHECK(packet.header.connection_token == config.connection_token);
    CHECK(packet.header.content_epoch == config.content_epoch);
    CHECK(packet.header.codec == config.codec);
    CHECK(packet.header.width == config.width);
    CHECK(packet.header.height == config.height);
    CHECK(packet.header.coded_width >= packet.header.width);
    CHECK(packet.header.coded_height >= packet.header.height);
    CHECK((packet.header.coded_width & 1u) == 0);
    CHECK((packet.header.coded_height & 1u) == 0);
    CHECK(packet.header.frame_id != 0);
    return true;
}

bool encode_until_packet(wd_video_encoder* encoder, const wd_video_encoder_config& config, std::vector<uint32_t>& pixels,
                         uint32_t first_frame_index, wd_video_encoder_packet& out_packet) {
    for (uint32_t attempt = 0; attempt < 12; ++attempt)
    {
        const uint32_t frame_index = first_frame_index + attempt;
        fill_pattern(pixels, frame_index);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels        = pixels.data();
        input.width         = kWidth;
        input.height        = kHeight;
        input.stride_pixels = kStride;
        input.pts_usec      = UINT64_C(1000000) + static_cast<uint64_t>(frame_index) * UINT64_C(33333);

        wd_video_encoder_packet packet{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &packet));
        if (packet.header.data_size != 0)
        {
            CHECK(packet_metadata_is_valid(packet, config));
            out_packet = packet;
            return true;
        }
    }
    return false;
}

bool test_invalid_api() {
    CHECK(!wd_video_encoder_create(nullptr, "software"));

    wd_video_encoder* encoder = nullptr;
    CHECK(!wd_video_encoder_create(&encoder, "invalid-backend"));
    CHECK(encoder == nullptr);
    CHECK(wd_video_encoder_backend_name(nullptr) != nullptr);
    CHECK(std::strcmp(wd_video_encoder_backend_name(nullptr), "none") == 0);
    CHECK(!wd_video_encoder_request_keyframe(nullptr));
    CHECK(!wd_video_encoder_configure(nullptr, nullptr));

    CHECK(wd_video_encoder_create(&encoder, "software"));
    CHECK(encoder != nullptr);
    CHECK(std::strcmp(wd_video_encoder_backend_name(encoder), "software") == 0);
    CHECK(wd_video_encoder_choose_codec(encoder, 0) == 0);
    CHECK(wd_video_encoder_choose_codec(encoder, UINT32_C(0x80000000)) == 0);

    wd_video_encoder_config invalid{};
    CHECK(!wd_video_encoder_configure(encoder, &invalid));
    invalid.codec = WD_VIDEO_CODEC_H264;
    CHECK(!wd_video_encoder_configure(encoder, &invalid));
    invalid.width = 16;
    CHECK(!wd_video_encoder_configure(encoder, &invalid));

    wd_video_encoder_input_xrgb8888 input{};
    wd_video_encoder_packet         packet{};
    CHECK(!wd_video_encoder_encode_xrgb8888(encoder, &input, &packet));
    CHECK(packet.header.data_size == 0);

    wd_video_encoder_destroy(encoder);
    wd_video_encoder_destroy(nullptr);
    return true;
}

bool test_codec(uint32_t codec) {
    wd_video_encoder* encoder = nullptr;
    CHECK(wd_video_encoder_create(&encoder, "software"));

    wd_video_encoder_config config{};
    config.session_id             = 7;
    config.connection_token       = kConnectionToken;
    config.content_epoch          = 3;
    config.width                  = kWidth;
    config.height                 = kHeight;
    config.target_fps             = 30;
    config.bitrate_kib_per_second = 2048;
    config.codec                  = codec;

    CHECK(wd_video_encoder_configure(encoder, &config));
    CHECK(wd_video_encoder_configure(encoder, &config));
    CHECK(std::strcmp(wd_video_encoder_backend_name(encoder), "software") != 0);

    std::vector<uint32_t>   pixels(static_cast<size_t>(kStride) * kHeight);
    wd_video_encoder_packet first{};
    CHECK(encode_until_packet(encoder, config, pixels, 0, first));
    CHECK((first.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
    CHECK((first.header.flags & WD_VIDEO_FRAME_CONFIG) != 0);
    CHECK(first.header.frame_id == 1);

    const uint64_t          first_frame_id = first.header.frame_id;
    wd_video_encoder_packet second{};
    CHECK(encode_until_packet(encoder, config, pixels, 20, second));
    CHECK(second.header.frame_id > first_frame_id);

    CHECK(wd_video_encoder_request_keyframe(encoder));
    bool saw_requested_keyframe = false;
    for (uint32_t attempt = 0; attempt < 12 && !saw_requested_keyframe; ++attempt)
    {
        wd_video_encoder_packet packet{};
        CHECK(encode_until_packet(encoder, config, pixels, 40 + attempt * 12u, packet));
        saw_requested_keyframe = (packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0;
    }
    CHECK(saw_requested_keyframe);

    wd_video_encoder_reset(encoder);
    fill_pattern(pixels, 100);
    wd_video_encoder_input_xrgb8888 input{};
    input.pixels        = pixels.data();
    input.width         = kWidth;
    input.height        = kHeight;
    input.stride_pixels = kStride;
    input.pts_usec      = UINT64_C(9000000);
    wd_video_encoder_packet after_reset{};
    CHECK(!wd_video_encoder_encode_xrgb8888(encoder, &input, &after_reset));

    config.content_epoch = 4;
    CHECK(wd_video_encoder_configure(encoder, &config));
    wd_video_encoder_packet new_epoch{};
    CHECK(encode_until_packet(encoder, config, pixels, 120, new_epoch));
    CHECK(new_epoch.header.frame_id == 1);
    CHECK(new_epoch.header.content_epoch == 4);

    wd_video_encoder_destroy(encoder);
    return true;
}

} // namespace

int main() {
    if (!test_invalid_api())
    {
        return 1;
    }

    wd_video_encoder* encoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "software"))
    {
        return 1;
    }
    const uint32_t supported = wd_video_encoder_supported_codecs(encoder);
    const uint32_t compiled  = compiled_codec_mask();
    if ((supported & ~compiled) != 0)
    {
        std::fprintf(stderr, "encoder reported codecs not enabled by this build: 0x%x\n", supported);
        wd_video_encoder_destroy(encoder);
        return 1;
    }
    if (wd_video_encoder_available(encoder) != (supported != 0))
    {
        wd_video_encoder_destroy(encoder);
        return 1;
    }

    const uint32_t chosen = wd_video_encoder_choose_codec(encoder, compiled);
    const uint32_t expected_choice =
        (supported & WD_VIDEO_CODEC_H265) != 0 ? WD_VIDEO_CODEC_H265 : ((supported & WD_VIDEO_CODEC_H264) != 0 ? WD_VIDEO_CODEC_H264 : 0);
    if (chosen != expected_choice)
    {
        std::fprintf(stderr, "unexpected codec choice: got=0x%x expected=0x%x\n", chosen, expected_choice);
        wd_video_encoder_destroy(encoder);
        return 1;
    }
    wd_video_encoder_destroy(encoder);

    if (supported == 0)
    {
        std::fprintf(stderr, "SKIP: FFmpeg exposes no enabled software video encoder\n");
        return 77;
    }

    if ((supported & WD_VIDEO_CODEC_H264) != 0 && !test_codec(WD_VIDEO_CODEC_H264))
    {
        return 1;
    }
    if ((supported & WD_VIDEO_CODEC_H265) != 0 && !test_codec(WD_VIDEO_CODEC_H265))
    {
        return 1;
    }

    return 0;
}
