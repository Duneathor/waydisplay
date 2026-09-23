#pragma once

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

/* The capped fast-link target is a budget, NOT an instruction to fill the link.
 * CQP keeps quantization predictable when a very large CBR/VBR target can
 * yield undesirable driver-specific rate-control choices. */
#define WD_VIDEO_HEVC_VAAPI_HIGH_BANDWIDTH_QP 18
static inline bool wd_video_hevc_vaapi_prefer_quality(uint32_t codec, uint32_t bitrate_kib_per_second) {
    return codec == WD_VIDEO_CODEC_H265 &&
           bitrate_kib_per_second >= WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND;
}
