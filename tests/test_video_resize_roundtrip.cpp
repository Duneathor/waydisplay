#include "video_decoder.hpp"
#include "video_packet_validation.h"
#include "wd_video_encoder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                    \
            return false;                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

uint16_t coded(uint16_t value) {
    return static_cast<uint16_t>((value + 1u) & ~1u);
}

void fill(std::vector<uint32_t>& pixels, uint16_t width, uint16_t height, uint32_t frame) {
    pixels.resize(static_cast<size_t>(width) * height);
    for (uint16_t y = 0; y < height; ++y)
    {
        for (uint16_t x = 0; x < width; ++x)
        {
            const uint32_t r = (x + frame * 7u) & 0xffu;
            const uint32_t g = (y * 3u + frame * 11u) & 0xffu;
            const uint32_t b = (x + y + frame * 13u) & 0xffu;
            pixels[static_cast<size_t>(y) * width + x] = UINT32_C(0xff000000) | (r << 16u) | (g << 8u) | b;
        }
    }
}

struct EncodedCopy {
    wd_video_frame_payload_header header{};
    std::vector<uint8_t> data;
};

bool configure(wd_video_encoder* encoder, waydisplay::ClientVideoDecoder* decoder, uint32_t codec, uint64_t epoch,
               uint16_t width, uint16_t height) {
    wd_video_encoder_config encoder_config{};
    encoder_config.session_id = 9;
    encoder_config.connection_token = UINT64_C(0x9988776655443322);
    encoder_config.content_epoch = epoch;
    encoder_config.width = width;
    encoder_config.height = height;
    encoder_config.target_fps = 30;
    encoder_config.bitrate_kib_per_second = 4096;
    encoder_config.codec = codec;
    CHECK(wd_video_encoder_configure(encoder, &encoder_config));
    CHECK(wd_video_encoder_request_keyframe(encoder));

    waydisplay::ClientVideoDecoderConfig decoder_config{};
    decoder_config.session_id = encoder_config.session_id;
    decoder_config.connection_token = encoder_config.connection_token;
    decoder_config.content_epoch = epoch;
    decoder_config.width = width;
    decoder_config.height = height;
    decoder_config.coded_width = coded(width);
    decoder_config.coded_height = coded(height);
    decoder_config.target_fps = 30;
    decoder_config.codec = codec;
    decoder_config.hwdecode_mode = WD_CLIENT_VIDEO_HWDECODE_OFF;
    CHECK(waydisplay::client_video_decoder_configure(decoder, decoder_config));
    return true;
}

bool encode_until_packet(wd_video_encoder* encoder, uint16_t width, uint16_t height, uint32_t frame_base, EncodedCopy& copy) {
    std::vector<uint32_t> pixels;
    for (uint32_t index = 0; index < 30; ++index)
    {
        fill(pixels, width, height, frame_base + index);
        wd_video_encoder_input_xrgb8888 input{};
        input.pixels = pixels.data();
        input.width = width;
        input.height = height;
        input.stride_pixels = width;
        input.pts_usec = UINT64_C(1000000) + static_cast<uint64_t>(frame_base + index) * UINT64_C(33333);
        wd_video_encoder_packet packet{};
        CHECK(wd_video_encoder_encode_xrgb8888(encoder, &input, &packet));
        if (packet.header.data_size == 0)
        {
            continue;
        }
        copy.header = packet.header;
        copy.data.assign(packet.data, packet.data + packet.header.data_size);
        return true;
    }
    return false;
}

bool decode_available(waydisplay::ClientVideoDecoder* decoder, const EncodedCopy& encoded, uint16_t width,
                      uint16_t height, uint64_t epoch, bool* saw_output) {
    CHECK(saw_output != nullptr);
    waydisplay::ClientVideoPacket packet{encoded.header, encoded.data.data()};
    waydisplay::ClientDecodedVideoFrame decoded{};
    CHECK(waydisplay::client_video_decoder_decode(decoder, packet, &decoded));
    for (;;)
    {
        if (decoded.format == waydisplay::ClientVideoPixelFormat::None)
        {
            break;
        }
        waydisplay::ClientVideoFrameBuffer output{};
        CHECK(waydisplay::client_video_decoder_swap_output_frame(decoder, output));
        CHECK(output.valid());
        CHECK(decoded.width == width && decoded.height == height && decoded.content_epoch == epoch);
        *saw_output = true;

        decoded = waydisplay::ClientDecodedVideoFrame{};
        if (!waydisplay::client_video_decoder_take_frame(decoder, &decoded))
        {
            break;
        }
    }
    return true;
}

bool run_codec(uint32_t codec) {
    wd_video_encoder* encoder = nullptr;
    waydisplay::ClientVideoDecoder* decoder = nullptr;
    CHECK(wd_video_encoder_create(&encoder, "software"));
    CHECK(waydisplay::client_video_decoder_create(&decoder));

    struct Geometry { uint16_t width; uint16_t height; };
    const Geometry geometries[] = {{320, 180}, {641, 361}, {320, 180}};
    EncodedCopy previous{};
    uint64_t epoch = 1;
    for (size_t transition = 0; transition < std::size(geometries); ++transition, ++epoch)
    {
        const auto geometry = geometries[transition];
        wd_video_encoder_reset(encoder);
        waydisplay::client_video_decoder_reset(decoder);
        CHECK(configure(encoder, decoder, codec, epoch, geometry.width, geometry.height));

        if (!previous.data.empty())
        {
            const wd_client_video_packet_expectation expected{previous.header.session_id, previous.header.connection_token,
                                                               geometry.width, geometry.height};
            bool control = false;
            CHECK(wd_client_video_packet_validate(&previous.header,
                                                  static_cast<uint32_t>(sizeof(previous.header) + previous.data.size()),
                                                  &expected, &control) == WD_CLIENT_VIDEO_PACKET_INVALID_GEOMETRY);
            CHECK(!control);
        }

        bool saw_keyframe = false;
        bool saw_output = false;
        for (uint32_t frame = 0; frame < 30 && !saw_output; ++frame)
        {
            EncodedCopy encoded{};
            CHECK(encode_until_packet(encoder, geometry.width, geometry.height,
                                      static_cast<uint32_t>(transition * 100 + frame), encoded));
            CHECK(encoded.header.content_epoch == epoch);
            CHECK(encoded.header.width == geometry.width && encoded.header.height == geometry.height);
            CHECK(encoded.header.coded_width == coded(geometry.width) && encoded.header.coded_height == coded(geometry.height));
            const wd_client_video_packet_expectation expected{encoded.header.session_id, encoded.header.connection_token,
                                                               geometry.width, geometry.height};
            bool control = false;
            CHECK(wd_client_video_packet_validate(&encoded.header,
                                                  static_cast<uint32_t>(sizeof(encoded.header) + encoded.data.size()),
                                                  &expected, &control) == WD_CLIENT_VIDEO_PACKET_VALID);
            CHECK(!control);
            if (!saw_keyframe)
            {
                CHECK(encoded.header.frame_id == 1);
                CHECK((encoded.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0);
                saw_keyframe = true;
            }
            CHECK(decode_available(decoder, encoded, geometry.width, geometry.height, epoch, &saw_output));
            previous = std::move(encoded);
        }
        CHECK(saw_keyframe && saw_output);
    }

    waydisplay::client_video_decoder_destroy(decoder);
    wd_video_encoder_destroy(encoder);
    return true;
}

} // namespace

int main() {
    wd_video_encoder* encoder = nullptr;
    waydisplay::ClientVideoDecoder* decoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "software") || !waydisplay::client_video_decoder_create(&decoder))
    {
        wd_video_encoder_destroy(encoder);
        waydisplay::client_video_decoder_destroy(decoder);
        return 1;
    }
    const uint32_t codecs = wd_video_encoder_supported_codecs(encoder) & waydisplay::client_video_decoder_supported_codecs(decoder);
    wd_video_encoder_destroy(encoder);
    waydisplay::client_video_decoder_destroy(decoder);
    if (codecs == 0)
    {
        return 77;
    }
    if ((codecs & WD_VIDEO_CODEC_H264) != 0 && !run_codec(WD_VIDEO_CODEC_H264))
    {
        return 1;
    }
    if ((codecs & WD_VIDEO_CODEC_H265) != 0 && !run_codec(WD_VIDEO_CODEC_H265))
    {
        return 1;
    }
    return 0;
}
