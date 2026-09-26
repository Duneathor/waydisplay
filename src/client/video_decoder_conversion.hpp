#pragma once

#include "video_decoder.hpp"
#include "video_plane_copy.hpp"

#include <cstddef>
#include <cstdint>

namespace waydisplay {

inline bool client_video_decoder_prepare_iyuv(ClientVideoFrameBuffer& output, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
    {
        output.clear();
        return false;
    }

    const uint32_t uv_width  = (width + 1u) / 2u;
    const uint32_t uv_height = (height + 1u) / 2u;
    const size_t y_size      = static_cast<size_t>(width) * height;
    const size_t uv_size     = static_cast<size_t>(uv_width) * uv_height;
    const size_t total_size  = y_size + uv_size * 2u;
    try
    {
        if (output.bytes.size() != total_size)
        {
            output.bytes.resize(total_size);
        }
    }
    catch (...)
    {
        output.clear();
        return false;
    }

    output.format   = ClientVideoPixelFormat::IYUV;
    output.width    = width;
    output.height   = height;
    output.y_pitch  = width;
    output.uv_pitch = uv_width;
    output.u_offset = y_size;
    output.v_offset = y_size + uv_size;
    return true;
}

inline bool client_video_decoder_copy_visible_planes(ClientVideoPlaneLayout layout, const uint8_t* const src[3],
                                                     const int src_stride[3], uint32_t coded_width,
                                                     uint32_t coded_height, uint32_t visible_width,
                                                     uint32_t visible_height, ClientVideoFrameBuffer& output) {
    if (coded_width < visible_width || coded_height < visible_height ||
        !client_video_decoder_prepare_iyuv(output, visible_width, visible_height))
    {
        return false;
    }

    uint8_t* dst[3] = {
        output.bytes.data(),
        output.bytes.data() + output.u_offset,
        output.bytes.data() + output.v_offset,
    };
    return client_copy_video_planes(layout, src, src_stride, dst, visible_width, visible_height);
}

inline void client_video_decoder_assign_metadata(const wd_video_frame_payload_header& header,
                                                 ClientDecodedVideoFrame& output) {
    output.format        = ClientVideoPixelFormat::IYUV;
    output.width         = header.width;
    output.height        = header.height;
    output.frame_id      = header.frame_id;
    output.content_epoch = header.content_epoch;
    output.pts_usec      = header.pts_usec;
}

} // namespace waydisplay
