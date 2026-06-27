#include "wd_channel_binding.h"

#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_dispatch.h"

#include <string.h>

static bool wd_aux_identity_matches(uint8_t session_id, uint64_t connection_token,
                                    const struct wd_aux_channel_policy* policy) {
    return policy && policy->session_id != 0 && policy->connection_token != 0 && session_id == policy->session_id &&
           connection_token == policy->connection_token;
}

enum wd_aux_channel_kind wd_aux_channel_validate_hello(uint16_t message_type, const void* payload, uint32_t payload_size,
                                                        const struct wd_aux_channel_policy* policy) {
    if (!payload || !policy ||
        !wd_protocol_message_allowed(message_type, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
                                     WD_PROTOCOL_CLIENT_TO_SERVER, payload_size))
    {
        return WD_AUX_CHANNEL_INVALID;
    }

    switch (message_type)
    {
    case WD_MSG_INPUT_CHANNEL_HELLO: {
        if (policy->input_bound || payload_size != sizeof(struct wd_input_channel_hello_payload))
        {
            return WD_AUX_CHANNEL_INVALID;
        }
        struct wd_input_channel_hello_payload hello;
        memcpy(&hello, payload, sizeof(hello));
        return wd_aux_identity_matches(hello.session_id, hello.connection_token, policy) ? WD_AUX_CHANNEL_INPUT
                                                                                         : WD_AUX_CHANNEL_INVALID;
    }
    case WD_MSG_SELECTION_CHANNEL_HELLO: {
        if (policy->selection_bound || payload_size != sizeof(struct wd_selection_channel_hello_payload))
        {
            return WD_AUX_CHANNEL_INVALID;
        }
        struct wd_selection_channel_hello_payload hello;
        memcpy(&hello, payload, sizeof(hello));
        return wd_aux_identity_matches(hello.session_id, hello.connection_token, policy) ? WD_AUX_CHANNEL_SELECTION
                                                                                         : WD_AUX_CHANNEL_INVALID;
    }
    case WD_MSG_VIDEO_CHANNEL_HELLO: {
        if (policy->video_bound || !policy->video_negotiated ||
            payload_size != sizeof(struct wd_video_channel_hello_payload))
        {
            return WD_AUX_CHANNEL_INVALID;
        }
        struct wd_video_channel_hello_payload hello;
        memcpy(&hello, payload, sizeof(hello));
        if (!wd_aux_identity_matches(hello.session_id, hello.connection_token, policy) ||
            (hello.video_codecs & ~WD_VIDEO_CODEC_MASK) != 0 ||
            (hello.video_codecs & policy->video_codecs & WD_VIDEO_CODEC_MASK) == 0 ||
            hello.video_transport != policy->video_transport)
        {
            return WD_AUX_CHANNEL_INVALID;
        }
        return WD_AUX_CHANNEL_VIDEO;
    }
    case WD_MSG_AUDIO_CHANNEL_HELLO: {
        if (policy->audio_bound || !policy->audio_negotiated ||
            payload_size != sizeof(struct wd_audio_channel_hello_payload))
        {
            return WD_AUX_CHANNEL_INVALID;
        }
        struct wd_audio_channel_hello_payload hello;
        memcpy(&hello, payload, sizeof(hello));
        if (!wd_aux_identity_matches(hello.session_id, hello.connection_token, policy) || hello.audio_codecs != policy->audio_codec ||
            hello.audio_transport != policy->audio_transport)
        {
            return WD_AUX_CHANNEL_INVALID;
        }
        return WD_AUX_CHANNEL_AUDIO;
    }
    default:
        return WD_AUX_CHANNEL_INVALID;
    }
}
