#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_video_encoder;

struct wd_video_encoder_config {
    uint8_t  session_id;
    uint64_t connection_token;
    uint64_t content_epoch;
    uint16_t width;
    uint16_t height;
    uint16_t target_fps;
    uint32_t bitrate_kib_per_second;
    uint32_t codec;
};

struct wd_video_encoder_input_xrgb8888 {
    const uint32_t* pixels;
    uint32_t        width;
    uint32_t        height;
    uint32_t        stride_pixels;
    uint64_t        pts_usec;
};

struct wd_video_encoder_packet {
    struct wd_video_frame_payload_header header;

    /* Owned by the encoder and valid until the next encode/reset/destroy call. */
    const uint8_t* data;
};

bool wd_video_encoder_create(struct wd_video_encoder** out_encoder,
                             const char* video_encoder_backend,
                             const char* vaapi_device);
void wd_video_encoder_destroy(struct wd_video_encoder* encoder);
void wd_video_encoder_reset(struct wd_video_encoder* encoder);

bool wd_video_encoder_available(const struct wd_video_encoder* encoder);
uint32_t wd_video_encoder_supported_codecs(const struct wd_video_encoder* encoder);
uint32_t wd_video_encoder_choose_codec(struct wd_video_encoder* encoder, uint32_t client_codecs);
const char* wd_video_encoder_backend_name(const struct wd_video_encoder* encoder);

bool wd_video_encoder_configure(struct wd_video_encoder* encoder, const struct wd_video_encoder_config* config);
bool wd_video_encoder_request_keyframe(struct wd_video_encoder* encoder);
bool wd_video_encoder_encode_xrgb8888(struct wd_video_encoder* encoder,
                                      const struct wd_video_encoder_input_xrgb8888* input,
                                      struct wd_video_encoder_packet* packet);

#ifdef __cplusplus
}
#endif
