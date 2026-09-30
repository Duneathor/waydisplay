#pragma once

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_client_video_offer {
    uint32_t capabilities;
    uint32_t codecs;
    uint8_t  transport;
};

static inline struct wd_client_video_offer wd_client_video_offer_decide(uint8_t video_mode,
                                                                         uint8_t decoder_mode,
                                                                         uint32_t supported_codecs,
                                                                         uint32_t requested_codecs) {
    struct wd_client_video_offer offer = {0, 0, 0};
    if (video_mode == WD_VIDEO_MODE_OFF || video_mode > WD_VIDEO_MODE_FORCE ||
        decoder_mode >= WD_CLIENT_VIDEO_DECODER_OFF)
    {
        return offer;
    }

    offer.codecs = (supported_codecs & requested_codecs & WD_VIDEO_CODEC_MASK);
    if (offer.codecs == 0)
    {
        return offer;
    }

    offer.capabilities = WD_CLIENT_CAP_VIDEO_STREAM | WD_CLIENT_CAP_VIDEO_FEEDBACK;
    offer.transport    = WD_VIDEO_TRANSPORT_TCP;
    return offer;
}

#ifdef __cplusplus
}
#endif
