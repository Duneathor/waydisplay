#include "video_packet_validation.h"

enum wd_client_video_packet_validation_result
wd_client_video_packet_validate(const struct wd_video_frame_payload_header* header, uint32_t payload_size,
                                const struct wd_client_video_packet_expectation* expected, bool* control_frame) {
    if (control_frame)
    {
        *control_frame = false;
    }
    if (!header || !expected ||
        (header->codec != WD_VIDEO_CODEC_H265 && header->codec != WD_VIDEO_CODEC_H264) ||
        !wd_video_frame_payload_size_is_valid(header, payload_size))
    {
        return WD_CLIENT_VIDEO_PACKET_INVALID_PAYLOAD;
    }

    const bool control = (header->flags & (WD_VIDEO_FRAME_END_OF_STREAM | WD_VIDEO_FRAME_RESIZE)) != 0;
    if (control_frame)
    {
        *control_frame = control;
    }
    if (header->data_size == 0 && !control)
    {
        return WD_CLIENT_VIDEO_PACKET_INVALID_PAYLOAD;
    }
    if (header->session_id != expected->session_id || header->connection_token != expected->connection_token)
    {
        return WD_CLIENT_VIDEO_PACKET_INVALID_IDENTITY;
    }
    if (!control && (header->width != expected->width || header->height != expected->height ||
                     header->coded_width < header->width || header->coded_height < header->height))
    {
        return WD_CLIENT_VIDEO_PACKET_INVALID_GEOMETRY;
    }
    return WD_CLIENT_VIDEO_PACKET_VALID;
}
