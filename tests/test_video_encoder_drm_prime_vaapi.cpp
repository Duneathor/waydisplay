#include "waydisplay/wd_frame.h"
#include "waydisplay/wd_protocol.h"
#include "wd_video_encoder.h"

#include <gbm.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {

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

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

uint32_t choose_codec(uint32_t supported) {
    if ((supported & WD_VIDEO_CODEC_H264) != 0)
    {
        return WD_VIDEO_CODEC_H264;
    }
    if ((supported & WD_VIDEO_CODEC_H265) != 0)
    {
        return WD_VIDEO_CODEC_H265;
    }
    if ((supported & WD_VIDEO_CODEC_AV1) != 0)
    {
        return WD_VIDEO_CODEC_AV1;
    }
    return 0;
}

bool fill_bo(gbm_bo* bo) {
    uint32_t map_stride = 0;
    void* map_data = nullptr;
    void* mapped = gbm_bo_map(bo, 0, 0, kWidth, kHeight, GBM_BO_TRANSFER_WRITE,
                              &map_stride, &map_data);
    if (!mapped)
    {
        return false;
    }

    for (uint32_t y = 0; y < kHeight; ++y)
    {
        auto* row = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * map_stride);
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t red = (x * 3u + y) & 0xffu;
            const uint32_t green = (y * 5u + x) & 0xffu;
            const uint32_t blue = (x ^ y) & 0xffu;
            row[x] = UINT32_C(0xff000000) | (red << 16u) | (green << 8u) | blue;
        }
    }
    gbm_bo_unmap(bo, map_data);
    return true;
}

bool encode_gbm_bo(gbm_bo* bo, uint32_t codec) {
    wd_video_encoder* encoder = nullptr;
    CHECK(wd_video_encoder_create(&encoder, "vaapi"));

    wd_video_encoder_config config{};
    config.session_id = 43;
    config.connection_token = UINT64_C(0x44524d5052494d45);
    config.content_epoch = 9;
    config.width = kWidth;
    config.height = kHeight;
    config.target_fps = 30;
    config.bitrate_kib_per_second = 8192;
    config.codec = codec;
    if (!wd_video_encoder_configure(encoder, &config) ||
        !wd_video_encoder_supports_drm_prime(encoder))
    {
        wd_video_encoder_destroy(encoder);
        return false;
    }

    const int prime_fd = gbm_bo_get_fd(bo);
    if (prime_fd < 0)
    {
        wd_video_encoder_destroy(encoder);
        return false;
    }

    wd_frame_drm_plane plane{};
    plane.fd = prime_fd;
    plane.stride = gbm_bo_get_stride(bo);
    plane.offset = 0;
    plane.modifier = gbm_bo_get_modifier(bo);

    wd_frame frame{};
    wd_frame_init(&frame);
    const bool frame_ready = wd_frame_set_drm_prime_dup(
        &frame, kWidth, kHeight, GBM_FORMAT_XRGB8888, UINT64_C(1000000), &plane, 1);
    close(prime_fd);
    if (!frame_ready)
    {
        wd_frame_reset(&frame);
        wd_video_encoder_destroy(encoder);
        return false;
    }

    CHECK(frame.storage == WD_FRAME_STORAGE_DRM_PRIME);
    CHECK(frame.data.drm.plane_count == 1);
    CHECK(frame.data.drm.planes[0].fd >= 0);

    bool produced_packet = false;
    for (uint32_t attempt = 0; attempt < 16; ++attempt)
    {
        frame.pts_usec = UINT64_C(1000000) + static_cast<uint64_t>(attempt) * UINT64_C(33333);
        wd_video_encoder_packet packet{};
        if (!wd_video_encoder_encode_frame(encoder, &frame, &packet))
        {
            wd_frame_reset(&frame);
            wd_video_encoder_destroy(encoder);
            return false;
        }
        if (packet.header.data_size != 0)
        {
            CHECK(packet.buffer != nullptr);
            CHECK(packet.data != nullptr);
            CHECK(packet.header.codec == codec);
            CHECK(packet.header.width == kWidth);
            CHECK(packet.header.height == kHeight);
            produced_packet = true;
            wd_video_encoder_packet_release(&packet);
            break;
        }
        wd_video_encoder_packet_release(&packet);
    }

    wd_frame_reset(&frame);
    wd_video_encoder_destroy(encoder);
    return produced_packet;
}

bool try_render_node(const std::string& path, uint32_t codec, bool& exercised_bo) {
    const int drm_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (drm_fd < 0)
    {
        return false;
    }

    gbm_device* device = gbm_create_device(drm_fd);
    if (!device)
    {
        close(drm_fd);
        return false;
    }

    const uint32_t usage_candidates[] = {
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR,
        GBM_BO_USE_RENDERING,
    };

    bool encoded = false;
    for (uint32_t usage : usage_candidates)
    {
        gbm_bo* bo = gbm_bo_create(device, kWidth, kHeight, GBM_FORMAT_XRGB8888, usage);
        if (!bo)
        {
            continue;
        }
        if (gbm_bo_get_stride(bo) >= kWidth * sizeof(uint32_t) && fill_bo(bo))
        {
            exercised_bo = true;
            encoded = encode_gbm_bo(bo, codec);
        }
        gbm_bo_destroy(bo);
        if (encoded)
        {
            break;
        }
    }

    gbm_device_destroy(device);
    close(drm_fd);
    return encoded;
}

} // namespace

int main() {
#if !defined(__linux__)
    return 77;
#else
    wd_video_encoder* probe = nullptr;
    if (!wd_video_encoder_create(&probe, "vaapi"))
    {
        return 77;
    }
    const uint32_t supported = wd_video_encoder_supported_codecs(probe);
    const uint32_t codec = choose_codec(supported);
    wd_video_encoder_destroy(probe);
    if (codec == 0)
    {
        std::fprintf(stderr, "SKIP: no VAAPI encoder available\n");
        return 77;
    }

    bool exercised_bo = false;
    for (unsigned node = 128; node < 192; ++node)
    {
        const std::string path = "/dev/dri/renderD" + std::to_string(node);
        if (try_render_node(path, codec, exercised_bo))
        {
            return 0;
        }
    }

    if (!exercised_bo)
    {
        std::fprintf(stderr, "SKIP: no CPU-mappable single-plane GBM XRGB8888 buffer available\n");
        return 77;
    }

    std::fprintf(stderr,
                 "DRM PRIME regression: GBM produced a valid dma-buf but VAAPI encode_frame rejected every render-node candidate\n");
    return 1;
#endif
}
