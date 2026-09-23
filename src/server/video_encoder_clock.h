#pragma once

#include <stdint.h>
#include "waydisplay/wd_config.h"

/* Codec configuration describes the session's nominal stream clock, not a
 * transient adaptive capture cap.  Changing the codec framerate reopens
 * FFmpeg/VA-API and discards its reference history.  Actual PTS are stamped
 * from the media clock; the compositor independently paces captured frames. */
static inline uint16_t wd_video_encoder_nominal_fps(uint16_t requested_session_fps) {
    if (requested_session_fps == 0)
    {
        return WD_DEFAULT_SESSION_FPS;
    }
    return requested_session_fps > WD_MAX_SESSION_FPS ? WD_MAX_SESSION_FPS : requested_session_fps;
}
