#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_aux_channel_kind {
    WD_AUX_CHANNEL_INVALID = 0,
    WD_AUX_CHANNEL_INPUT,
    WD_AUX_CHANNEL_SELECTION,
    WD_AUX_CHANNEL_VIDEO,
    WD_AUX_CHANNEL_AUDIO,
};

struct wd_aux_channel_policy {
    uint8_t  session_id;
    uint64_t connection_token;

    bool input_bound;
    bool selection_bound;
    bool video_bound;
    bool audio_bound;

    bool     video_negotiated;
    uint32_t video_codecs;
    uint16_t video_transport;

    bool     audio_negotiated;
    uint32_t audio_codec;
    uint16_t audio_transport;
};

/* Validate one auxiliary-channel hello without mutating socket state. */
enum wd_aux_channel_kind wd_aux_channel_validate_hello(uint16_t message_type, const void* payload, uint32_t payload_size,
                                                        const struct wd_aux_channel_policy* policy);

#ifdef __cplusplus
}
#endif
