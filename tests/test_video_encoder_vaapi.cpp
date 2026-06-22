#include "wd_video_encoder.h"

#include "vaapi_test_device.hpp"
#include "waydisplay/wd_protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);   \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

void fill_pattern(std::vector<uint32_t>& pixels, uint32_t frame_number) {
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t red = (x + frame_number * 13u) & 0xffu;
            const uint32_t green = (y + frame_number * 19u) & 0xffu;
            const uint32_t blue = (x ^ y ^ (frame_number * 7u)) & 0xffu;
            pixels[static_cast<size_t>(y) * kWidth + x] =
                UINT32_C(0xff000000) | (red << 16u) | (green << 8u) | blue;
        }
    }
}

bool test_unavailable_device_paths() {
    constexpr const char* missing_device = "/dev/waydisplay-vaapi-device-does-not-exist";

    wd_video_encoder* forced = nullptr;
    CHECK(wd_video_encoder_create(&forced, "vaapi", missing_device));
    CHECK(forced != nullptr);
    CHECK(std::strcmp(wd_video_encoder_backend_name(forced), "vaapi") == 0);
    CHECK(!wd_video_encoder_available(forced));
    CHECK(wd_video_encoder_supported_codecs(forced) == 0);

    wd_video_encoder_config config{};
    config.session_id = 1;
    config.connection_token = 2;
    config.content_epoch = 3;
    config.width = kWidth;
    config.height = kHeight;
    config.target_fps = 30;
    config.bitrate_kib_per_second = 4096;
    config.codec = WD_VIDEO_CODEC_H264;
    CHECK(!wd_video_encoder_configure(forced, &config));
    wd_video_encoder_destroy(forced);

    wd_video_encoder* automatic = nullptr;
    CHECK(wd_video_encoder_create(&automatic, "auto", missing_device));
    const uint32_t software_codecs = wd_video_encoder_supported_codecs(automatic);
    if (software_codecs != 0)
    {
        config.codec = (software_codecs & WD_VIDEO_CODEC_H265) != 0
                           ? WD_VIDEO_CODEC_H265
                           : WD_VIDEO_CODEC_H264;
        CHECK(wd_video_encoder_configure(automatic, &config));
        CHECK(std::strstr(wd_video_encoder_backend_name(automatic), "vaapi") == nullptr);
    }
    wd_video_encoder_destroy(automatic);
    return true;
}

bool encode_hardware_codec(wd_video_encoder* encoder, uint32_t codec) {
    wd_video_encoder_config config{};
    config.session_id = 5;
    config.connection_token = UINT64_C(0x5051525354555657);
    config.content_epoch = 8;
    config.width = kWidth;
    config.height = kHeight;
    config.target_fps = 30;
    config.bitrate_kib_per_second = 8192;
    config.codec = codec;
    CHECK(wd_video_encoder_configure(encoder, &config));
    CHECK(std::strstr(wd_video_encoder_backend_name(encoder), "vaapi") != nullptr);

    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
    bool produced_packet = false;
    for (uint32_t frame_number = 0; frame_number < 16 && !produced_packet; ++frame_number)
    {
        fill_pattern(pixels, frame_number);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels = pixels.data();
        input.width = kWidth;
        input.height = kHeight;
        input.stride_pixels = kWidth;
        input.pts_usec = UINT64_C(1000000) + static_cast<uint64_t>(frame_number) * UINT64_C(33333);

        wd_video_encoder_packet packet{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &packet));
        if (packet.header.data_size == 0)
        {
            continue;
        }
        CHECK(packet.data != nullptr);
        CHECK(packet.header.codec == codec);
        CHECK(packet.header.width == kWidth);
        CHECK(packet.header.height == kHeight);
        CHECK((packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
        CHECK(wd_video_frame_payload_size_is_valid(
            &packet.header,
            static_cast<uint32_t>(sizeof(packet.header)) + packet.header.data_size));
        produced_packet = true;
    }
    CHECK(produced_packet);
    return true;
}

} // namespace

int main() {
    const std::optional<std::string> device = waydisplay::test::find_vaapi_test_device();
    if (!device)
    {
        std::fprintf(stderr, "SKIP: no usable VAAPI render node\n");
        return 77;
    }
    std::fprintf(stderr, "VAAPI encoder test device: %s\n", device->c_str());

    wd_video_encoder* encoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "vaapi", device->c_str()))
    {
        return 1;
    }
    const uint32_t supported = wd_video_encoder_supported_codecs(encoder);
    if (supported == 0)
    {
        std::fprintf(stderr, "SKIP: no usable VAAPI video encoder on %s\n", device->c_str());
        wd_video_encoder_destroy(encoder);
        return 77;
    }

    if ((supported & WD_VIDEO_CODEC_H264) != 0 &&
        !encode_hardware_codec(encoder, WD_VIDEO_CODEC_H264))
    {
        wd_video_encoder_destroy(encoder);
        return 1;
    }
    wd_video_encoder_reset(encoder);
    if ((supported & WD_VIDEO_CODEC_H265) != 0 &&
        !encode_hardware_codec(encoder, WD_VIDEO_CODEC_H265))
    {
        wd_video_encoder_destroy(encoder);
        return 1;
    }

    wd_video_encoder_destroy(encoder);
    return test_unavailable_device_paths() ? 0 : 1;
}
