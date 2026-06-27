#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_client_video_packet_validation_result {
    WD_CLIENT_VIDEO_PACKET_VALID = 0,
    WD_CLIENT_VIDEO_PACKET_INVALID_PAYLOAD,
    WD_CLIENT_VIDEO_PACKET_INVALID_IDENTITY,
    WD_CLIENT_VIDEO_PACKET_INVALID_GEOMETRY,
};

struct wd_client_video_packet_expectation {
    uint8_t  session_id;
    uint64_t connection_token;
    uint16_t width;
    uint16_t height;
};

enum wd_client_video_packet_validation_result
wd_client_video_packet_validate(const struct wd_video_frame_payload_header* header, uint32_t payload_size,
                                const struct wd_client_video_packet_expectation* expected, bool* control_frame);

#ifdef __cplusplus
}
#endif
