#pragma once

#include <algorithm>
#include <cstdint>

namespace waydisplay {

/* Integer destination pixels. Input coordinates are handled separately in
 * logical window coordinates, which may differ on high-DPI displays. */
struct ClientVideoPresentationRect {
    int  x = 0;
    int  y = 0;
    int  w = 1;
    int  h = 1;
    bool pixel_exact = false;
};

inline ClientVideoPresentationRect client_video_presentation_rect(int output_width, int output_height, uint32_t source_width,
                                                                uint32_t source_height) {
    output_width = std::max(1, output_width);
    output_height = std::max(1, output_height);
    if (source_width == 0 || source_height == 0)
    {
        return {0, 0, output_width, output_height, false};
    }
    if (source_width == static_cast<uint32_t>(output_width) && source_height == static_cast<uint32_t>(output_height))
    {
        return {0, 0, output_width, output_height, true};
    }

    int w = output_width;
    int h = output_height;
    const uint64_t width_limited_height = static_cast<uint64_t>(output_width) * source_height / source_width;
    if (width_limited_height <= static_cast<uint64_t>(output_height))
    {
        h = std::max(1, static_cast<int>(width_limited_height));
    }
    else
    {
        w = std::max(1, static_cast<int>(static_cast<uint64_t>(output_height) * source_width / source_height));
    }
    return {(output_width - w) / 2, (output_height - h) / 2, w, h, false};
}

} // namespace waydisplay
