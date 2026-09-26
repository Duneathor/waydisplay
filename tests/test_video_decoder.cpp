#include "video_decoder.hpp"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_net.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef WAYDISPLAY_TEST_HAVE_H264_DECODER
#define WAYDISPLAY_TEST_HAVE_H264_DECODER 0
#endif
#ifndef WAYDISPLAY_TEST_HAVE_AV1_DECODER
#define WAYDISPLAY_TEST_HAVE_AV1_DECODER 0
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

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                                             \
            return false;                                                                                                                  \
        }                                                                                                                                  \
    } while (false)

uint32_t compiled_codec_mask() {
    uint32_t mask = 0;
#if WAYDISPLAY_TEST_HAVE_H264_DECODER
    mask |= WD_VIDEO_CODEC_H264;
#endif
#if WAYDISPLAY_TEST_HAVE_H265_DECODER
    mask |= WD_VIDEO_CODEC_H265;
#endif
#if WAYDISPLAY_TEST_HAVE_AV1_DECODER
    mask |= WD_VIDEO_CODEC_AV1;
#endif
    return mask;
}

const char* aggregate_backend_name(uint32_t codecs) {
    if (codecs == WD_VIDEO_CODEC_MASK)
    {
        return "h264/hevc/av1";
    }
    if (codecs == (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_AV1))
    {
        return "h264/av1";
    }
    if (codecs == (WD_VIDEO_CODEC_H265 | WD_VIDEO_CODEC_AV1))
    {
        return "hevc/av1";
    }
    if ((codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265)) == (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265))
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
    if ((codecs & WD_VIDEO_CODEC_AV1) != 0)
    {
        return "av1";
    }
    return "none";
}

std::vector<uint8_t> read_fixture(const char* name) {
    const std::string path = std::string(WAYDISPLAY_TEST_FIXTURE_DIR) + "/" + name;
    std::ifstream     input(path, std::ios::binary);
    if (!input)
    {
        std::fprintf(stderr, "failed to open video fixture: %s\n", path.c_str());
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

struct OwnedInput {
    wd_buffer* buffer = nullptr;

    OwnedInput() = default;
    OwnedInput(const OwnedInput&) = delete;
    OwnedInput& operator=(const OwnedInput&) = delete;

    ~OwnedInput() {
        wd_buffer_release(buffer);
    }

    bool assign(ClientVideoPacket& packet, const uint8_t* bytes, size_t size, size_t prefix, bool padded = true,
                bool poison_padding = false) {
        wd_buffer_release(buffer);
        buffer = nullptr;
        const size_t logical = prefix + size;
        buffer = padded ? wd_buffer_alloc_padded(logical, WD_TCP_PAYLOAD_PADDING_BYTES) : wd_buffer_alloc(logical);
        if (!buffer)
        {
            return false;
        }
        uint8_t* storage = wd_buffer_data(buffer);
        if (prefix != 0)
        {
            std::memset(storage, 0x5c, prefix);
        }
        std::memcpy(storage + prefix, bytes, size);
        if (poison_padding)
        {
            if (wd_buffer_capacity(buffer) <= logical)
            {
                return false;
            }
            storage[logical] = 0x7f;
        }
        packet.data  = storage + prefix;
        packet.owner = buffer;
        return true;
    }

    void release_caller_reference(ClientVideoPacket& packet) {
        wd_buffer_release(buffer);
        buffer       = nullptr;
        packet.owner = nullptr;
        packet.data  = nullptr;
    }
};

struct FixtureAccessUnit {
    size_t   size     = 0;
    uint64_t pts_usec = 0;
};

std::vector<FixtureAccessUnit> read_manifest(const char* name) {
    const std::string path = std::string(WAYDISPLAY_TEST_FIXTURE_DIR) + "/" + name;
    std::ifstream     input(path);
    if (!input)
    {
        std::fprintf(stderr, "failed to open video fixture manifest: %s\n", path.c_str());
        return {};
    }

    std::vector<FixtureAccessUnit> access_units;
    std::string                    line;
    while (std::getline(input, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        std::istringstream fields(line);
        FixtureAccessUnit  access_unit{};
        if (!(fields >> access_unit.size >> access_unit.pts_usec) || access_unit.size == 0)
        {
            std::fprintf(stderr, "invalid video fixture manifest line: %s\n", line.c_str());
            return {};
        }
        access_units.push_back(access_unit);
    }
    return access_units;
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
    CHECK(waydisplay::client_video_decoder_zero_copy_inputs(nullptr) == 0);
    CHECK(waydisplay::client_video_decoder_copied_inputs(nullptr) == 0);
    CHECK(!waydisplay::client_video_decoder_configure(nullptr, ClientVideoDecoderConfig{}));

    ClientVideoDecoder* decoder = nullptr;
    CHECK(waydisplay::client_video_decoder_create(&decoder));
    CHECK(decoder != nullptr);
    CHECK(std::strcmp(waydisplay::client_video_decoder_backend_name(nullptr),
                      aggregate_backend_name(waydisplay::client_video_decoder_supported_codecs(decoder))) == 0);

    ClientVideoDecoderConfig invalid{};
    CHECK(!waydisplay::client_video_decoder_configure(decoder, invalid));
    ClientVideoDecoderConfig off_config{};
    off_config.codec        = WD_VIDEO_CODEC_H264;
    off_config.width        = 64;
    off_config.height       = 48;
    off_config.coded_width  = 64;
    off_config.coded_height = 48;
    off_config.decode_mode  = WD_CLIENT_VIDEO_DECODER_OFF;
    CHECK(!waydisplay::client_video_decoder_configure(decoder, off_config));
    off_config.decode_mode = UINT8_MAX;
    CHECK(!waydisplay::client_video_decoder_configure(decoder, off_config));

    invalid.codec = WD_VIDEO_CODEC_H264;
    invalid.width = 64;
    CHECK(!waydisplay::client_video_decoder_configure(decoder, invalid));
    invalid.height       = 48;
    invalid.coded_width  = 63;
    invalid.coded_height = 48;
    CHECK(!waydisplay::client_video_decoder_configure(decoder, invalid));

    ClientVideoPacket       packet{};
    ClientDecodedVideoFrame frame{};
    CHECK(!waydisplay::client_video_decoder_decode(decoder, packet, &frame));
    CHECK(frame.format == ClientVideoPixelFormat::None);

    ClientVideoFrameBuffer output{};
    CHECK(!waydisplay::client_video_decoder_swap_output_frame(decoder, output));
    CHECK(!waydisplay::client_video_decoder_take_frame(decoder, &frame));
    CHECK(frame.format == ClientVideoPixelFormat::None);
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
    config.session_id       = 9;
    config.connection_token = UINT64_C(0x8899aabbccddeeff);
    config.content_epoch    = 12;
    config.width            = 63;
    config.height           = 47;
    config.coded_width      = 64;
    config.coded_height     = 48;
    config.target_fps       = 30;
    config.codec            = codec;
    config.decode_mode      = WD_CLIENT_VIDEO_DECODER_SOFTWARE;

    CHECK(waydisplay::client_video_decoder_configure(decoder, config));
    CHECK(waydisplay::client_video_decoder_configure(decoder, config));
    CHECK(!waydisplay::client_video_decoder_hwdecode_failed_auto(decoder));
    CHECK(std::strcmp(waydisplay::client_video_decoder_backend_name(decoder), "none") != 0);
#if WAYDISPLAY_TEST_HAVE_AV1_DECODER
    if (codec == WD_VIDEO_CODEC_AV1)
    {
        const char* backend = waydisplay::client_video_decoder_backend_name(decoder);
        CHECK(std::strcmp(backend, "libdav1d") == 0 || std::strcmp(backend, "libaom-av1") == 0);
    }
#endif

    ClientVideoPacket packet{};
    packet.header.session_id       = config.session_id;
    packet.header.connection_token = config.connection_token;
    packet.header.content_epoch    = config.content_epoch;
    packet.header.codec            = codec;
    packet.header.flags            = WD_VIDEO_FRAME_CONFIG | WD_VIDEO_FRAME_KEYFRAME;
    packet.header.frame_id         = 41;
    packet.header.pts_usec         = UINT64_C(1234567);
    packet.header.width            = config.width;
    packet.header.height           = config.height;
    packet.header.coded_width      = config.coded_width;
    packet.header.coded_height     = config.coded_height;
    packet.header.data_size        = static_cast<uint32_t>(fixture.size());
    OwnedInput owned_input{};
    CHECK(owned_input.assign(packet, fixture.data(), fixture.size(), sizeof(packet.header) + 7u));
    CHECK(wd_video_frame_payload_size_is_valid(&packet.header, static_cast<uint32_t>(sizeof(packet.header) + fixture.size())));

    ClientDecodedVideoFrame frame{};
    CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &frame));
    CHECK(waydisplay::client_video_decoder_zero_copy_inputs(decoder) == 1);
    CHECK(waydisplay::client_video_decoder_copied_inputs(decoder) == 0);
    owned_input.release_caller_reference(packet);
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

    const size_t y_size    = static_cast<size_t>(output.y_pitch) * output.height;
    const size_t uv_height = (output.height + 1u) / 2u;
    const size_t uv_size   = static_cast<size_t>(output.uv_pitch) * uv_height;
    CHECK(output.u_offset == y_size);
    CHECK(output.v_offset == y_size + uv_size);
    CHECK(output.bytes.size() == y_size + uv_size * 2u);
    CHECK(plane_has_variation(output.bytes.data(), y_size));
    CHECK(plane_has_variation(output.bytes.data() + output.u_offset, uv_size) ||
          plane_has_variation(output.bytes.data() + output.v_offset, uv_size));
    CHECK(!waydisplay::client_video_decoder_swap_output_frame(decoder, output));
    CHECK(!waydisplay::client_video_decoder_take_frame(decoder, &frame));

    /* An owner without FFmpeg's required tail padding must use the safe copy
     * path rather than exposing an over-readable network buffer. */
    waydisplay::client_video_decoder_reset(decoder);
    CHECK(waydisplay::client_video_decoder_configure(decoder, config));
    ClientVideoPacket fallback = packet;
    fallback.data               = nullptr;
    fallback.owner              = nullptr;
    OwnedInput unpadded{};
    CHECK(unpadded.assign(fallback, fixture.data(), fixture.size(), 3u, false));
    ClientDecodedVideoFrame fallback_frame{};
    CHECK(waydisplay::client_video_decoder_decode(decoder, fallback, &fallback_frame));
    CHECK(waydisplay::client_video_decoder_zero_copy_inputs(decoder) == 1);
    CHECK(waydisplay::client_video_decoder_copied_inputs(decoder) == 1);
    unpadded.release_caller_reference(fallback);
    if (fallback_frame.format != ClientVideoPixelFormat::None)
    {
        ClientVideoFrameBuffer fallback_output{};
        CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, fallback_output));
        CHECK(fallback_output.valid());
    }

    /* Padding must be zero, not merely present. Poisoned tail bytes also force
     * the copy path, whose av_new_packet storage restores the padding contract. */
    waydisplay::client_video_decoder_reset(decoder);
    CHECK(waydisplay::client_video_decoder_configure(decoder, config));
    ClientVideoPacket poisoned = packet;
    poisoned.data               = nullptr;
    poisoned.owner              = nullptr;
    OwnedInput poisoned_input{};
    CHECK(poisoned_input.assign(poisoned, fixture.data(), fixture.size(), 0u, true, true));
    ClientDecodedVideoFrame poisoned_frame{};
    CHECK(waydisplay::client_video_decoder_decode(decoder, poisoned, &poisoned_frame));
    CHECK(waydisplay::client_video_decoder_zero_copy_inputs(decoder) == 1);
    CHECK(waydisplay::client_video_decoder_copied_inputs(decoder) == 2);
    poisoned_input.release_caller_reference(poisoned);
    if (poisoned_frame.format != ClientVideoPixelFormat::None)
    {
        ClientVideoFrameBuffer poisoned_output{};
        CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, poisoned_output));
        CHECK(poisoned_output.valid());
    }

    waydisplay::client_video_decoder_reset(decoder);
    CHECK(!waydisplay::client_video_decoder_decode(decoder, packet, nullptr));
    waydisplay::client_video_decoder_destroy(decoder);
    return true;
}

bool collect_decoded_frames(ClientVideoDecoder* decoder, ClientDecodedVideoFrame frame, std::vector<ClientDecodedVideoFrame>& frames) {
    for (;;)
    {
        if (frame.format == ClientVideoPixelFormat::None)
        {
            return true;
        }

        ClientVideoFrameBuffer output{};
        CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
        CHECK(output.valid());
        CHECK(output.width == frame.width);
        CHECK(output.height == frame.height);
        frames.push_back(frame);

        frame = ClientDecodedVideoFrame{};
        if (!waydisplay::client_video_decoder_take_frame(decoder, &frame))
        {
            return true;
        }
    }
}

bool test_delayed_codec(uint32_t codec, const char* fixture_name, const char* manifest_name) {
    const std::vector<uint8_t>           fixture      = read_fixture(fixture_name);
    const std::vector<FixtureAccessUnit> access_units = read_manifest(manifest_name);
    CHECK(!fixture.empty());
    CHECK(access_units.size() >= 8);

    size_t fixture_size = 0;
    for (const FixtureAccessUnit& access_unit : access_units)
    {
        CHECK(access_unit.size <= fixture.size() - fixture_size);
        fixture_size += access_unit.size;
    }
    CHECK(fixture_size == fixture.size());

    ClientVideoDecoder* decoder = nullptr;
    CHECK(waydisplay::client_video_decoder_create(&decoder));

    ClientVideoDecoderConfig config{};
    config.session_id       = 17;
    config.connection_token = UINT64_C(0x1718191a1b1c1d1e);
    config.content_epoch    = 23;
    config.width            = 64;
    config.height           = 48;
    config.coded_width      = 64;
    config.coded_height     = 48;
    config.target_fps       = 4;
    config.codec            = codec;
    config.decode_mode      = WD_CLIENT_VIDEO_DECODER_SOFTWARE;
    CHECK(waydisplay::client_video_decoder_configure(decoder, config));

    constexpr uint64_t                   kPtsBase           = UINT64_C(4000000);
    constexpr uint64_t                   kFrameDurationUsec = UINT64_C(250000);
    std::vector<ClientDecodedVideoFrame> decoded_frames;
    size_t                               offset = 0;
    for (size_t index = 0; index < access_units.size(); ++index)
    {
        const FixtureAccessUnit& access_unit = access_units[index];
        ClientVideoPacket        packet{};
        packet.header.session_id       = config.session_id;
        packet.header.connection_token = config.connection_token;
        packet.header.content_epoch    = config.content_epoch;
        packet.header.codec            = codec;
        packet.header.flags            = index == 0 ? WD_VIDEO_FRAME_CONFIG | WD_VIDEO_FRAME_KEYFRAME : 0;
        packet.header.frame_id         = access_unit.pts_usec / kFrameDurationUsec + 1u;
        packet.header.pts_usec         = kPtsBase + access_unit.pts_usec;
        packet.header.width            = config.width;
        packet.header.height           = config.height;
        packet.header.coded_width      = config.coded_width;
        packet.header.coded_height     = config.coded_height;
        packet.header.data_size        = static_cast<uint32_t>(access_unit.size);
        OwnedInput owned_input{};
        CHECK(owned_input.assign(packet, fixture.data() + offset, access_unit.size, sizeof(packet.header)));
        offset += access_unit.size;

        ClientDecodedVideoFrame frame{};
        CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &frame));
        owned_input.release_caller_reference(packet);
        CHECK(collect_decoded_frames(decoder, frame, decoded_frames));
    }

    CHECK(decoded_frames.size() >= 8);
    for (size_t index = 0; index < decoded_frames.size(); ++index)
    {
        const ClientDecodedVideoFrame& frame = decoded_frames[index];
        CHECK(frame.format == ClientVideoPixelFormat::IYUV);
        CHECK(frame.width == config.width);
        CHECK(frame.height == config.height);
        CHECK(frame.content_epoch == config.content_epoch);
        CHECK(frame.frame_id == index + 1u);
        CHECK(frame.pts_usec == kPtsBase + index * kFrameDurationUsec);
    }
    ClientDecodedVideoFrame frame{};
    CHECK(!waydisplay::client_video_decoder_take_frame(decoder, &frame));
    CHECK(waydisplay::client_video_decoder_zero_copy_inputs(decoder) == access_units.size());
    CHECK(waydisplay::client_video_decoder_copied_inputs(decoder) == 0);

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
    const uint32_t compiled  = compiled_codec_mask();
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

    if ((supported & WD_VIDEO_CODEC_H264) != 0 && !test_codec(WD_VIDEO_CODEC_H264, "video_keyframe_64x48.h264"))
    {
        return 1;
    }
    if ((supported & WD_VIDEO_CODEC_H264) != 0 &&
        !test_delayed_codec(WD_VIDEO_CODEC_H264, "video_delayed_64x48.h264", "video_delayed_64x48.h264.manifest"))
    {
        return 1;
    }
    if ((supported & WD_VIDEO_CODEC_H265) != 0 && !test_codec(WD_VIDEO_CODEC_H265, "video_keyframe_64x48.h265"))
    {
        return 1;
    }
    if ((supported & WD_VIDEO_CODEC_H265) != 0 &&
        !test_delayed_codec(WD_VIDEO_CODEC_H265, "video_delayed_64x48.h265", "video_delayed_64x48.h265.manifest"))
    {
        return 1;
    }
#if WAYDISPLAY_TEST_HAVE_AV1_DECODER
    /* This decoder test is software-only; fail if a hardware-only FFmpeg av1
     * implementation is accidentally used for explicit software decoding. */
    if ((supported & WD_VIDEO_CODEC_AV1) != 0 &&
        !test_codec(WD_VIDEO_CODEC_AV1, "video_keyframe_64x48.obu"))
    {
        return 1;
    }
#endif
    return 0;
}
