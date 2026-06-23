#include "wd_audio_encoder.h"

#include "waydisplay/wd_protocol.h"

#include <stdlib.h>

#ifndef WAYDISPLAY_HAVE_OPUS_AUDIO
#define WAYDISPLAY_HAVE_OPUS_AUDIO 0
#endif

#if WAYDISPLAY_HAVE_OPUS_AUDIO
#include <opus/opus.h>

struct wd_audio_encoder {
    OpusEncoder* opus;
    uint8_t      channels;
    uint16_t     delay_samples;
};

bool wd_audio_encoder_create(struct wd_audio_encoder** out_encoder, uint8_t channels, uint32_t bitrate) {
    if (!out_encoder || channels == 0 || channels > WD_AUDIO_CHANNELS_MAX || bitrate == 0)
    {
        return false;
    }
    *out_encoder                     = NULL;
    struct wd_audio_encoder* encoder = calloc(1, sizeof(*encoder));
    if (!encoder)
    {
        return false;
    }
    int error     = OPUS_OK;
    encoder->opus = opus_encoder_create(WD_AUDIO_SAMPLE_RATE_DEFAULT, channels, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder->opus || error != OPUS_OK)
    {
        free(encoder);
        return false;
    }
    encoder->channels = channels;
    if (opus_encoder_ctl(encoder->opus, OPUS_SET_BITRATE((opus_int32)bitrate)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_VBR(1)) != OPUS_OK || opus_encoder_ctl(encoder->opus, OPUS_SET_DTX(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_INBAND_FEC(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder->opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) != OPUS_OK)
    {
        wd_audio_encoder_destroy(encoder);
        return false;
    }
    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(encoder->opus, OPUS_GET_LOOKAHEAD(&lookahead)) == OPUS_OK && lookahead > 0 && lookahead <= UINT16_MAX)
    {
        encoder->delay_samples = (uint16_t)lookahead;
    }
    *out_encoder = encoder;
    return true;
}

void wd_audio_encoder_destroy(struct wd_audio_encoder* encoder) {
    if (!encoder)
    {
        return;
    }
    if (encoder->opus)
    {
        opus_encoder_destroy(encoder->opus);
    }
    free(encoder);
}

bool wd_audio_encoder_reset(struct wd_audio_encoder* encoder) {
    return encoder && encoder->opus && opus_encoder_ctl(encoder->opus, OPUS_RESET_STATE) == OPUS_OK;
}

bool wd_audio_encoder_encode(struct wd_audio_encoder* encoder, const float* interleaved, uint16_t frame_samples, uint8_t* output,
                             uint32_t output_capacity, uint32_t* output_size) {
    if (output_size)
    {
        *output_size = 0;
    }
    if (!encoder || !encoder->opus || !interleaved || !output || !output_size || !wd_audio_frame_samples_is_valid(frame_samples) ||
        output_capacity == 0 || output_capacity > INT32_MAX)
    {
        return false;
    }
    const opus_int32 encoded = opus_encode_float(encoder->opus, interleaved, frame_samples, output, (opus_int32)output_capacity);
    if (encoded <= 0)
    {
        return false;
    }
    *output_size = (uint32_t)encoded;
    return true;
}

uint16_t wd_audio_encoder_delay_samples(const struct wd_audio_encoder* encoder) {
    return encoder ? encoder->delay_samples : 0;
}

bool wd_audio_encoder_available(void) {
    return true;
}

const char* wd_audio_encoder_backend_name(void) {
    return "libopus";
}

#else

struct wd_audio_encoder {
    int unused;
};

bool wd_audio_encoder_create(struct wd_audio_encoder** out_encoder, uint8_t channels, uint32_t bitrate) {
    (void)channels;
    (void)bitrate;
    if (out_encoder)
    {
        *out_encoder = NULL;
    }
    return false;
}

void wd_audio_encoder_destroy(struct wd_audio_encoder* encoder) {
    free(encoder);
}

bool wd_audio_encoder_reset(struct wd_audio_encoder* encoder) {
    (void)encoder;
    return false;
}

bool wd_audio_encoder_encode(struct wd_audio_encoder* encoder, const float* interleaved, uint16_t frame_samples, uint8_t* output,
                             uint32_t output_capacity, uint32_t* output_size) {
    (void)encoder;
    (void)interleaved;
    (void)frame_samples;
    (void)output;
    (void)output_capacity;
    if (output_size)
    {
        *output_size = 0;
    }
    return false;
}

uint16_t wd_audio_encoder_delay_samples(const struct wd_audio_encoder* encoder) {
    (void)encoder;
    return 0;
}

bool wd_audio_encoder_available(void) {
    return false;
}

const char* wd_audio_encoder_backend_name(void) {
    return "unavailable";
}

#endif
