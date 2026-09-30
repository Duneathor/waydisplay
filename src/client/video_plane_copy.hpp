#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace waydisplay {

enum class ClientVideoPlaneLayout : uint8_t {
    YUV420P,
    NV12,
};

/* Copy only the visible picture, not any codec padding. Refuse unusual strides
 * so that the caller can use sws_scale for those decoded frames instead. */
inline bool client_copy_video_planes(ClientVideoPlaneLayout layout, const uint8_t* const src[3], const int src_stride[3],
                                     uint8_t* const dst[3], uint32_t width, uint32_t height) {
    if ((layout != ClientVideoPlaneLayout::YUV420P && layout != ClientVideoPlaneLayout::NV12) ||
        width == 0 || height == 0 || !src || !src_stride || !dst || !src[0] || !src[1] || !dst[0] || !dst[1] || !dst[2])
    {
        return false;
    }
    const uint32_t uv_width  = width / 2u + width % 2u;
    const uint32_t uv_height = height / 2u + height % 2u;
    if (width > static_cast<uint32_t>(INT_MAX) || uv_width > static_cast<uint32_t>(INT_MAX) ||
        (layout == ClientVideoPlaneLayout::NV12 && uv_width > static_cast<uint32_t>(INT_MAX / 2)) ||
        static_cast<size_t>(width) > SIZE_MAX / static_cast<size_t>(height) ||
        static_cast<size_t>(uv_width) > SIZE_MAX / static_cast<size_t>(uv_height) ||
        src_stride[0] < static_cast<int>(width) || src_stride[1] < static_cast<int>(layout == ClientVideoPlaneLayout::NV12 ?
                                                                                    uv_width * 2u : uv_width) ||
        (layout == ClientVideoPlaneLayout::YUV420P && (!src[2] || src_stride[2] < static_cast<int>(uv_width))))
    {
        return false;
    }

    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(dst[0] + static_cast<size_t>(y) * width,
                    src[0] + static_cast<size_t>(y) * static_cast<size_t>(src_stride[0]), width);
    }
    for (uint32_t y = 0; y < uv_height; ++y)
    {
        uint8_t* const u = dst[1] + static_cast<size_t>(y) * uv_width;
        uint8_t* const v = dst[2] + static_cast<size_t>(y) * uv_width;
        const uint8_t* const chroma = src[1] + static_cast<size_t>(y) * static_cast<size_t>(src_stride[1]);
        if (layout == ClientVideoPlaneLayout::YUV420P)
        {
            std::memcpy(u, chroma, uv_width);
            std::memcpy(v, src[2] + static_cast<size_t>(y) * static_cast<size_t>(src_stride[2]), uv_width);
        }
        else
        {
            for (uint32_t x = 0; x < uv_width; ++x)
            {
                u[x] = chroma[2u * x];
                v[x] = chroma[2u * x + 1u];
            }
        }
    }
    return true;
}

} // namespace waydisplay
