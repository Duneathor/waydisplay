#include "waydisplay/wd_config.h"
#include "waydisplay/wd_frame.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"
#include "wd_video_encoder.h"

#include <gbm.h>

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

uint32_t choose_codec(uint32_t supported) {
    if ((supported & WD_VIDEO_CODEC_H264) != 0)
        return WD_VIDEO_CODEC_H264;
    if ((supported & WD_VIDEO_CODEC_H265) != 0)
        return WD_VIDEO_CODEC_H265;
    if ((supported & WD_VIDEO_CODEC_AV1) != 0)
        return WD_VIDEO_CODEC_AV1;
    return 0;
}

bool fill_bo(gbm_bo* bo) {
    uint32_t map_stride = 0;
    void* map_data = nullptr;
    void* mapped = gbm_bo_map(bo, 0, 0, kWidth, kHeight, GBM_BO_TRANSFER_WRITE,
                              &map_stride, &map_data);
    if (!mapped)
        return false;

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

bool encode_bo(wd_video_encoder* encoder, gbm_bo* bo, uint32_t codec) {
    const int prime_fd = gbm_bo_get_fd(bo);
    if (prime_fd < 0)
        return false;

    wd_frame_drm_plane plane{};
    plane.fd = prime_fd;
    plane.stride = gbm_bo_get_stride(bo);
    plane.offset = 0;
    plane.modifier = gbm_bo_get_modifier(bo);

    wd_frame frame{};
    wd_frame_init(&frame);
    const bool ready = wd_frame_set_drm_prime_dup(
        &frame, kWidth, kHeight, GBM_FORMAT_XRGB8888, UINT64_C(1000000), &plane, 1);
    close(prime_fd);
    if (!ready)
        return false;

    bool produced = false;
    for (uint32_t attempt = 0; attempt < 16; ++attempt)
    {
        frame.pts_usec = UINT64_C(1000000) + static_cast<uint64_t>(attempt) * UINT64_C(33333);
        wd_video_encoder_packet packet{};
        if (!wd_video_encoder_encode_frame(encoder, &frame, &packet))
        {
            wd_frame_reset(&frame);
            return false;
        }
        if (packet.header.data_size != 0)
        {
            produced = packet.buffer != nullptr && packet.data != nullptr &&
                       packet.header.codec == codec;
            wd_video_encoder_packet_release(&packet);
            break;
        }
        wd_video_encoder_packet_release(&packet);
    }
    wd_frame_reset(&frame);
    return produced;
}

} // namespace

int main() {
#if !defined(__linux__)
    return 77;
#else
    wd_log_set_verbose(true);
    wd_video_encoder* encoder = nullptr;
    if (!wd_video_encoder_create(&encoder, "vaapi"))
        return 77;

    const uint32_t codec = choose_codec(wd_video_encoder_supported_codecs(encoder));
    if (codec == 0)
    {
        wd_video_encoder_destroy(encoder);
        return 77;
    }

    wd_video_encoder_config config{};
    config.session_id = 44;
    config.connection_token = UINT64_C(0x53414d4544455649);
    config.content_epoch = 10;
    config.width = kWidth;
    config.height = kHeight;
    config.target_fps = 30;
    config.bitrate_kib_per_second = WD_VIDEO_ENCODER_VAAPI_PROBE_BITRATE_KIB;
    config.codec = codec;
    if (!wd_video_encoder_configure(encoder, &config) || !wd_video_encoder_supports_drm_prime(encoder))
    {
        wd_video_encoder_destroy(encoder);
        return 77;
    }

    const char* selected = wd_video_encoder_vaapi_device_path(encoder);
    if (!selected || selected[0] == '\0')
    {
        std::fprintf(stderr, "FAIL: configured VAAPI encoder did not expose its selected render node\n");
        wd_video_encoder_destroy(encoder);
        return 1;
    }
    std::fprintf(stderr, "DRM PRIME same-device diagnostic: encoder_node=%s codec=0x%x\n",
                 selected, codec);

    const int drm_fd = open(selected, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0)
    {
        std::perror("open selected VAAPI render node");
        wd_video_encoder_destroy(encoder);
        return 1;
    }
    gbm_device* device = gbm_create_device(drm_fd);
    if (!device)
    {
        close(drm_fd);
        wd_video_encoder_destroy(encoder);
        return 1;
    }

    const uint32_t usages[] = {
        GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR,
        GBM_BO_USE_RENDERING,
    };
    bool exercised = false;
    bool encoded = false;
    for (uint32_t usage : usages)
    {
        gbm_bo* bo = gbm_bo_create(device, kWidth, kHeight, GBM_FORMAT_XRGB8888, usage);
        if (!bo)
            continue;
        if (gbm_bo_get_stride(bo) >= kWidth * sizeof(uint32_t) && fill_bo(bo))
        {
            exercised = true;
            std::fprintf(stderr,
                         "  same-device BO usage=0x%x stride=%u modifier=0x%016llx\n",
                         usage, gbm_bo_get_stride(bo),
                         static_cast<unsigned long long>(gbm_bo_get_modifier(bo)));
            encoded = encode_bo(encoder, bo, codec);
        }
        gbm_bo_destroy(bo);
        if (encoded)
            break;
    }

    gbm_device_destroy(device);
    close(drm_fd);
    wd_video_encoder_destroy(encoder);

    if (!exercised)
    {
        std::fprintf(stderr, "SKIP: selected VAAPI node could not create a CPU-mappable XRGB8888 GBM buffer\n");
        return 77;
    }
    if (!encoded)
    {
        std::fprintf(stderr, "FAIL: same-device GBM dma-buf still failed the production VAAPI VPP/encode path\n");
        return 1;
    }
    return 0;
#endif
}
