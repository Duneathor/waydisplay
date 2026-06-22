#include "video_decoder.hpp"

#include "waydisplay/wd_protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef WAYDISPLAY_TEST_HAVE_H264_DECODER
#define WAYDISPLAY_TEST_HAVE_H264_DECODER 0
#endif
#ifndef WAYDISPLAY_TEST_HAVE_H265_DECODER
#define WAYDISPLAY_TEST_HAVE_H265_DECODER 0
#endif
#ifndef WAYDISPLAY_TEST_FIXTURE_DIR
#define WAYDISPLAY_TEST_FIXTURE_DIR "."
#endif

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

uint32_t compiled_codec_mask() {
    uint32_t mask = 0;
#if WAYDISPLAY_TEST_HAVE_H264_DECODER
    mask |= WD_VIDEO_CODEC_H264;
#endif
#if WAYDISPLAY_TEST_HAVE_H265_DECODER
    mask |= WD_VIDEO_CODEC_H265;
#endif
    return mask;
}

const char* aggregate_backend_name(uint32_t codecs) {
    if ((codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265)) ==
        (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265))
    {
        return "h264/hevc";
    }
    if ((codecs & WD_VIDEO_CODEC_H264) != 0)
    {
        return "h264";
    }
    if ((codecs & WD_VIDEO_CODEC_H265) != 0)
    {
        return "hevc";
    }
    return "none";
}

std::vector<uint8_t> read_fixture(const char* name) {
    const std::string path = std::string(WAYDISPLAY_TEST_FIXTURE_DIR) + "/" + name;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        std::fprintf(stderr, "failed to open video fixture: %s\n", path.c_str());
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

bool plane_has_variation(const uint8_t* data, size_t size) {
    if (!data || size < 2)
    {
        return false;
    }
    const auto [minimum, maximum] = std::minmax_element(data, data + size);
    return *minimum != *maximum;
}

bool test_invalid_api() {
    CHECK(!waydisplay::client_video_decoder_create(nullptr));
    CHECK(!waydisplay::client_video_decoder_available(nullptr));
    CHECK(waydisplay::client_video_decoder_supported_codecs(nullptr) == 0);
    CHECK(waydisplay::client_video_decoder_backend_name(nullptr) != nullptr);
    CHECK(!waydisplay::client_video_decoder_hwdecode_failed_auto(nullptr));
    CHECK(!waydisplay::client_video_decoder_configure(nullptr, ClientVideoDecoderConfig{}));

    ClientVideoDecoder* decoder = nullptr;
    CHECK(waydisplay::client_video_decoder_create(&decoder));
    CHECK(decoder != nullptr);
    CHECK(std::strcmp(waydisplay::client_video_decoder_backend_name(nullptr),
                      aggregate_backend_name(
                          waydisplay::client_video_decoder_supported_codecs(decoder))) == 0);

    ClientVideoDecoderConfig invalid{};
    CHECK(!waydisplay::client_video_decoder_configure(decoder, invalid));
    invalid.codec = WD_VIDEO_CODEC_H264;
    invalid.width = 64;
    CHECK(!waydisplay::client_video_decoder_configure(decoder, invalid));
    invalid.height = 48;
    invalid.coded_width = 63;
    invalid.coded_height = 48;
    CHECK(!waydisplay::client_video_decoder_configure(decoder, invalid));

    ClientVideoPacket packet{};
    ClientDecodedVideoFrame frame{};
    CHECK(!waydisplay::client_video_decoder_decode(decoder, packet, &frame));
    CHECK(frame.format == ClientVideoPixelFormat::None);

    ClientVideoFrameBuffer output{};
    CHECK(!waydisplay::client_video_decoder_swap_output_frame(decoder, output));
    waydisplay::client_video_decoder_reset(decoder);
    waydisplay::client_video_decoder_reset(nullptr);
    waydisplay::client_video_decoder_destroy(decoder);
    waydisplay::client_video_decoder_destroy(nullptr);
    return true;
}

bool test_codec(uint32_t codec, const char* fixture_name) {
    const std::vector<uint8_t> fixture = read_fixture(fixture_name);
    CHECK(!fixture.empty());
    CHECK(fixture.size() <= WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES);

    ClientVideoDecoder* decoder = nullptr;
    CHECK(waydisplay::client_video_decoder_create(&decoder));

    ClientVideoDecoderConfig config{};
    config.session_id = 9;
    config.connection_token = UINT64_C(0x8899aabbccddeeff);
    config.content_epoch = 12;
    config.width = 63;
    config.height = 47;
    config.coded_width = 64;
    config.coded_height = 48;
    config.target_fps = 30;
    config.codec = codec;
    config.hwdecode_mode = WD_CLIENT_VIDEO_HWDECODE_OFF;

    CHECK(waydisplay::client_video_decoder_configure(decoder, config));
    CHECK(waydisplay::client_video_decoder_configure(decoder, config));
    CHECK(!waydisplay::client_video_decoder_hwdecode_failed_auto(decoder));
    CHECK(std::strcmp(waydisplay::client_video_decoder_backend_name(decoder), "none") != 0);

    ClientVideoPacket packet{};
    packet.header.session_id = config.session_id;
    packet.header.connection_token = config.connection_token;
    packet.header.content_epoch = config.content_epoch;
    packet.header.codec = codec;
    packet.header.flags = WD_VIDEO_FRAME_CONFIG | WD_VIDEO_FRAME_KEYFRAME;
    packet.header.frame_id = 41;
    packet.header.pts_usec = UINT64_C(1234567);
    packet.header.width = config.width;
    packet.header.height = config.height;
    packet.header.coded_width = config.coded_width;
    packet.header.coded_height = config.coded_height;
    packet.header.data_size = static_cast<uint32_t>(fixture.size());
    packet.data = fixture.data();
    CHECK(wd_video_frame_payload_size_is_valid(
        &packet.header, static_cast<uint32_t>(sizeof(packet.header) + fixture.size())));

    ClientDecodedVideoFrame frame{};
    CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &frame));
    CHECK(frame.format == ClientVideoPixelFormat::IYUV);
    CHECK(frame.width == config.width);
    CHECK(frame.height == config.height);
    CHECK(frame.frame_id == packet.header.frame_id);
    CHECK(frame.content_epoch == config.content_epoch);
    CHECK(frame.pts_usec == packet.header.pts_usec);

    ClientVideoFrameBuffer output{};
    CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
    CHECK(output.valid());
    CHECK(output.width == config.width);
    CHECK(output.height == config.height);
    CHECK(output.format == ClientVideoPixelFormat::IYUV);

    const size_t y_size = static_cast<size_t>(output.y_pitch) * output.height;
    const size_t uv_height = (output.height + 1u) / 2u;
    const size_t uv_size = static_cast<size_t>(output.uv_pitch) * uv_height;
    CHECK(output.u_offset == y_size);
    CHECK(output.v_offset == y_size + uv_size);
    CHECK(output.bytes.size() == y_size + uv_size * 2u);
    CHECK(plane_has_variation(output.bytes.data(), y_size));
    CHECK(plane_has_variation(output.bytes.data() + output.u_offset, uv_size) ||
          plane_has_variation(output.bytes.data() + output.v_offset, uv_size));
    CHECK(!waydisplay::client_video_decoder_swap_output_frame(decoder, output));

    waydisplay::client_video_decoder_reset(decoder);
    CHECK(!waydisplay::client_video_decoder_decode(decoder, packet, nullptr));
    waydisplay::client_video_decoder_destroy(decoder);
    return true;
}

} // namespace

int main() {
    if (!test_invalid_api())
    {
        return 1;
    }

    ClientVideoDecoder* decoder = nullptr;
    if (!waydisplay::client_video_decoder_create(&decoder))
    {
        return 1;
    }
    const uint32_t supported = waydisplay::client_video_decoder_supported_codecs(decoder);
    const uint32_t compiled = compiled_codec_mask();
    if ((supported & ~compiled) != 0)
    {
        std::fprintf(stderr, "decoder reported codecs not enabled by this build: 0x%x\n", supported);
        waydisplay::client_video_decoder_destroy(decoder);
        return 1;
    }
    if (waydisplay::client_video_decoder_available(decoder) != (supported != 0))
    {
        waydisplay::client_video_decoder_destroy(decoder);
        return 1;
    }
    waydisplay::client_video_decoder_destroy(decoder);

    if (supported == 0)
    {
        std::fprintf(stderr, "SKIP: FFmpeg exposes no enabled software video decoder\n");
        return 77;
    }

    if ((supported & WD_VIDEO_CODEC_H264) != 0 &&
        !test_codec(WD_VIDEO_CODEC_H264, "video_keyframe_64x48.h264"))
    {
        return 1;
    }
    if ((supported & WD_VIDEO_CODEC_H265) != 0 &&
        !test_codec(WD_VIDEO_CODEC_H265, "video_keyframe_64x48.h265"))
    {
        return 1;
    }
    return 0;
}
