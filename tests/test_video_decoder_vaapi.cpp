#include "video_decoder.hpp"
#include "waydisplay/wd_protocol.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

constexpr uint16_t kVaapiFixtureWidth  = 128;
constexpr uint16_t kVaapiFixtureHeight = 128;

std::vector<uint8_t> read_fixture(const char* name) {
    const std::string path = std::string(WAYDISPLAY_TEST_FIXTURE_DIR) + "/" + name;
    std::ifstream     input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

AVCodecID av_codec_id(uint32_t codec) {
    return codec == WD_VIDEO_CODEC_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
}

bool decoder_advertises_vaapi(uint32_t codec) {
    const AVCodec* decoder = avcodec_find_decoder(av_codec_id(codec));
    if (!decoder)
    {
        return false;
    }
    for (int index = 0;; ++index)
    {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
        if (!config)
        {
            break;
        }
        if (config->device_type == AV_HWDEVICE_TYPE_VAAPI && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
        {
            return true;
        }
    }
    return false;
}

ClientVideoDecoderConfig decoder_config(uint32_t codec, uint8_t hwdecode_mode) {
    ClientVideoDecoderConfig config{};
    config.session_id       = 13;
    config.connection_token = UINT64_C(0x131415161718191a);
    config.content_epoch    = 21;
    config.width            = kVaapiFixtureWidth;
    config.height           = kVaapiFixtureHeight;
    config.coded_width      = kVaapiFixtureWidth;
    config.coded_height     = kVaapiFixtureHeight;
    config.target_fps       = 30;
    config.codec            = codec;
    config.hwdecode_mode    = hwdecode_mode;
    return config;
}

enum class VaapiDecodeResult {
    Unsupported,
    Passed,
    Failed,
};

VaapiDecodeResult decode_with_vaapi(uint32_t codec, const char* fixture_name) {
    const std::vector<uint8_t> fixture = read_fixture(fixture_name);
    if (fixture.empty())
    {
        std::fprintf(stderr, "missing VAAPI decoder fixture: %s\n", fixture_name);
        return VaapiDecodeResult::Failed;
    }

    ClientVideoDecoder* decoder = nullptr;
    if (!waydisplay::client_video_decoder_create(&decoder))
    {
        return VaapiDecodeResult::Failed;
    }
    if (!waydisplay::client_video_decoder_configure(decoder, decoder_config(codec, WD_CLIENT_VIDEO_HWDECODE_VAAPI)))
    {
        waydisplay::client_video_decoder_destroy(decoder);
        return VaapiDecodeResult::Unsupported;
    }

    /* Hardware decoders may accept the first access unit without immediately
     * returning a displayable frame. Submit a short sequence of independent
     * keyframes, as the real client does, instead of treating normal pipeline
     * latency as a decode failure. */
    constexpr uint32_t kMaxAccessUnits       = 8;
    uint64_t           produced_output_count = 0;
    for (uint32_t attempt = 0; attempt < kMaxAccessUnits; ++attempt)
    {
        ClientVideoPacket packet{};
        packet.header.session_id       = 13;
        packet.header.connection_token = UINT64_C(0x131415161718191a);
        packet.header.content_epoch    = 21;
        packet.header.codec            = codec;
        packet.header.flags            = WD_VIDEO_FRAME_CONFIG | WD_VIDEO_FRAME_KEYFRAME;
        packet.header.frame_id         = static_cast<uint64_t>(attempt) + 1u;
        packet.header.pts_usec         = UINT64_C(2000000) + static_cast<uint64_t>(attempt) * UINT64_C(33333);
        packet.header.width            = kVaapiFixtureWidth;
        packet.header.height           = kVaapiFixtureHeight;
        packet.header.coded_width      = kVaapiFixtureWidth;
        packet.header.coded_height     = kVaapiFixtureHeight;
        packet.header.data_size        = static_cast<uint32_t>(fixture.size());
        packet.data                    = fixture.data();
        if (!wd_video_frame_payload_size_is_valid(&packet.header, static_cast<uint32_t>(sizeof(packet.header) + fixture.size())))
        {
            std::fprintf(stderr, "invalid VAAPI decoder fixture packet: %s\n", fixture_name);
            waydisplay::client_video_decoder_destroy(decoder);
            return VaapiDecodeResult::Failed;
        }

        ClientDecodedVideoFrame decoded{};
        if (!waydisplay::client_video_decoder_decode(decoder, packet, &decoded))
        {
            std::fprintf(stderr, "VAAPI decoder rejected access unit %u for codec 0x%x\n", attempt + 1u, codec);
            waydisplay::client_video_decoder_destroy(decoder);
            return VaapiDecodeResult::Failed;
        }

        for (;;)
        {
            if (decoded.format == ClientVideoPixelFormat::None)
            {
                break;
            }

            ClientVideoFrameBuffer output{};
            if (!waydisplay::client_video_decoder_swap_output_frame(decoder, output))
            {
                std::fprintf(stderr, "VAAPI decoder exposed metadata without a frame for codec 0x%x\n", codec);
                waydisplay::client_video_decoder_destroy(decoder);
                return VaapiDecodeResult::Failed;
            }

            const uint64_t expected_frame_id = produced_output_count + 1u;
            const uint64_t expected_pts      = UINT64_C(2000000) + produced_output_count * UINT64_C(33333);
            if (!output.valid() || decoded.format != ClientVideoPixelFormat::IYUV || decoded.width != kVaapiFixtureWidth ||
                decoded.height != kVaapiFixtureHeight || decoded.frame_id != expected_frame_id || decoded.content_epoch != 21 ||
                decoded.pts_usec != expected_pts ||
                std::strstr(waydisplay::client_video_decoder_backend_name(decoder), "vaapi") == nullptr ||
                waydisplay::client_video_decoder_hwdecode_failed_auto(decoder))
            {
                std::fprintf(stderr,
                             "VAAPI decoder produced invalid output for codec 0x%x: "
                             "backend=%s output=%s decoded=%ux%u frame=%llu pts=%llu\n",
                             codec, waydisplay::client_video_decoder_backend_name(decoder), output.valid() ? "valid" : "invalid",
                             decoded.width, decoded.height, static_cast<unsigned long long>(decoded.frame_id),
                             static_cast<unsigned long long>(decoded.pts_usec));
                waydisplay::client_video_decoder_destroy(decoder);
                return VaapiDecodeResult::Failed;
            }

            produced_output_count++;
            decoded = ClientDecodedVideoFrame{};
            if (!waydisplay::client_video_decoder_take_frame(decoder, &decoded))
            {
                break;
            }
        }
    }

    if (produced_output_count == 0)
    {
        std::fprintf(stderr,
                     "VAAPI decoder accepted %u access units but produced no frame for codec 0x%x "
                     "(backend=%s)\n",
                     kMaxAccessUnits, codec, waydisplay::client_video_decoder_backend_name(decoder));
    }

    waydisplay::client_video_decoder_destroy(decoder);
    return produced_output_count != 0 ? VaapiDecodeResult::Passed : VaapiDecodeResult::Failed;
}

} // namespace

int main() {
    ClientVideoDecoder* decoder = nullptr;
    if (!waydisplay::client_video_decoder_create(&decoder))
    {
        return 1;
    }
    const uint32_t supported = waydisplay::client_video_decoder_supported_codecs(decoder);
    waydisplay::client_video_decoder_destroy(decoder);
    if (supported == 0)
    {
        std::fprintf(stderr, "SKIP: no enabled decoder codec is available\n");
        return 77;
    }

    bool attempted = false;
    bool decoded   = false;
    if ((supported & WD_VIDEO_CODEC_H264) != 0 && decoder_advertises_vaapi(WD_VIDEO_CODEC_H264))
    {
        attempted                      = true;
        const VaapiDecodeResult result = decode_with_vaapi(WD_VIDEO_CODEC_H264, "video_keyframe_128x128.h264");
        if (result == VaapiDecodeResult::Failed)
        {
            return 1;
        }
        decoded = decoded || result == VaapiDecodeResult::Passed;
    }
    if ((supported & WD_VIDEO_CODEC_H265) != 0 && decoder_advertises_vaapi(WD_VIDEO_CODEC_H265))
    {
        attempted                      = true;
        const VaapiDecodeResult result = decode_with_vaapi(WD_VIDEO_CODEC_H265, "video_keyframe_128x128.h265");
        if (result == VaapiDecodeResult::Failed)
        {
            return 1;
        }
        decoded = decoded || result == VaapiDecodeResult::Passed;
    }

    if (!attempted || !decoded)
    {
        std::fprintf(stderr, "SKIP: VAAPI device does not decode an enabled test codec\n");
        return 77;
    }

    return 0;
}
