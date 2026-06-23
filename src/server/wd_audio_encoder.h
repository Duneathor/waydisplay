#pragma once

#include <stdbool.h>
#include <stdint.h>

struct wd_audio_encoder;

bool        wd_audio_encoder_create(struct wd_audio_encoder** out_encoder, uint8_t channels, uint32_t bitrate);
void        wd_audio_encoder_destroy(struct wd_audio_encoder* encoder);
bool        wd_audio_encoder_reset(struct wd_audio_encoder* encoder);
bool        wd_audio_encoder_encode(struct wd_audio_encoder* encoder, const float* interleaved, uint16_t frame_samples, uint8_t* output,
                                    uint32_t output_capacity, uint32_t* output_size);
uint16_t    wd_audio_encoder_delay_samples(const struct wd_audio_encoder* encoder);
bool        wd_audio_encoder_available(void);
const char* wd_audio_encoder_backend_name(void);
