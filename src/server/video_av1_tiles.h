#pragma once

#include <stdint.h>

/* libaom's default single AV1 tile leaves some of the four realtime
 * software-encoder threads idle on desktop-sized captures. Keep the
 * small test and low-resolution streams single-tile. Tile dimensions
 * are expressed as columns x rows (the FFmpeg libaom `tiles` option).
 * This only changes AV1 software encoding, never VAAPI or HEVC/H.264. */
static inline const char* wd_video_av1_software_tiles(uint32_t width, uint32_t height) {
    if (width >= 1280u && height >= 720u)
    {
        return "2x2";
    }
    if (width >= 768u && height >= 432u)
    {
        return "2x1";
    }
    return "1x1";
}
