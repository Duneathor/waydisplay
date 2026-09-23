#include "video_keyframe_recovery.h"

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stddef.h>

static size_t wd_start_code_size(const uint8_t* data, size_t size, size_t pos) {
    if (pos > size || size - pos < 3 || data[pos] != 0 || data[pos + 1] != 0)
    {
        return 0;
    }
    if (data[pos + 2] == 1)
    {
        return 3;
    }
    return size - pos >= 4 && data[pos + 2] == 0 && data[pos + 3] == 1 ? 4 : 0;
}

enum wd_client_video_keyframe_result wd_client_video_keyframe_validate(uint32_t codec, const uint8_t* data, uint32_t size) {
    if (!data || size == 0 || (codec != WD_VIDEO_CODEC_H264 && codec != WD_VIDEO_CODEC_H265))
    {
        return WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM;
    }

    const bool hevc = codec == WD_VIDEO_CODEC_H265;
    const unsigned required = hevc ? 0x07u : 0x03u;
    unsigned present = 0;
    bool random_access = false;
    size_t pos = 0;

    while (pos < size)
    {
        size_t prefix = wd_start_code_size(data, size, pos);
        if (prefix == 0)
        {
            /* Leading Annex-B zero_byte/leading_zero_8bits are legal, but
             * arbitrary bytes (including length-prefixed NALs) are not. */
            if (data[pos] != 0)
            {
                return WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM;
            }
            ++pos;
            continue;
        }

        const size_t nal = pos + prefix;
        size_t next = nal;
        while (next < size && wd_start_code_size(data, size, next) == 0)
        {
            ++next;
        }
        if (next - nal < (hevc ? 3u : 2u))
        {
            return WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM;
        }

        const uint8_t type = hevc ? (uint8_t)((data[nal] >> 1) & 0x3fu) : (uint8_t)(data[nal] & 0x1fu);
        if (hevc)
        {
            if (type == 32) present |= 0x01u; /* VPS */
            if (type == 33) present |= 0x02u; /* SPS */
            if (type == 34) present |= 0x04u; /* PPS */
            if (type <= 31)
            {
                /* A fresh decoder cannot use a dependent picture first. */
                if ((present & required) != required || (type != 19 && type != 20 && type != 21))
                {
                    return (present & required) != required ? WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS
                                                           : WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS;
                }
                random_access = true;
            }
        }
        else
        {
            if (type == 7) present |= 0x01u; /* SPS */
            if (type == 8) present |= 0x02u; /* PPS */
            if (type == 1 || type == 5)
            {
                if ((present & required) != required || type != 5)
                {
                    return (present & required) != required ? WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS
                                                           : WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS;
                }
                random_access = true;
            }
        }
        pos = next;
    }

    if ((present & required) != required)
    {
        return WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS;
    }
    return random_access ? WD_CLIENT_VIDEO_KEYFRAME_VALID : WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS;
}

const char* wd_client_video_keyframe_result_name(enum wd_client_video_keyframe_result result) {
    switch (result)
    {
    case WD_CLIENT_VIDEO_KEYFRAME_VALID:
        return "valid";
    case WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM:
        return "not Annex-B or truncated NAL";
    case WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS:
        return "missing codec parameter sets";
    case WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS:
        return "missing random-access picture";
    default:
        return "unknown";
    }
}
