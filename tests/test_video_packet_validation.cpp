#include "video_packet_validation.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                    \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

wd_video_frame_payload_header valid_header() {
    wd_video_frame_payload_header header{};
    header.session_id = 7;
    header.connection_token = 0x1122334455667788ull;
    header.content_epoch = 4;
    header.codec = WD_VIDEO_CODEC_H265;
    header.flags = WD_VIDEO_FRAME_KEYFRAME;
    header.frame_id = 1;
    header.width = 800;
    header.height = 600;
    header.coded_width = 800;
    header.coded_height = 600;
    header.data_size = 16;
    return header;
}

void test_payload_identity_and_geometry() {
    const wd_client_video_packet_expectation expected{7, 0x1122334455667788ull, 800, 600};
    auto header = valid_header();
    bool control = true;
    CHECK(wd_client_video_packet_validate(&header, sizeof(header) + header.data_size, &expected, &control) ==
          WD_CLIENT_VIDEO_PACKET_VALID);
    CHECK(!control);

    header.connection_token++;
    CHECK(wd_client_video_packet_validate(&header, sizeof(header) + header.data_size, &expected, &control) ==
          WD_CLIENT_VIDEO_PACKET_INVALID_IDENTITY);
    header = valid_header();
    header.width = 1366;
    header.height = 717;
    header.coded_width = 1366;
    header.coded_height = 718;
    CHECK(wd_client_video_packet_validate(&header, sizeof(header) + header.data_size, &expected, &control) ==
          WD_CLIENT_VIDEO_PACKET_INVALID_GEOMETRY);

    header = valid_header();
    header.coded_width = 799;
    CHECK(wd_client_video_packet_validate(&header, sizeof(header) + header.data_size, &expected, &control) ==
          WD_CLIENT_VIDEO_PACKET_INVALID_PAYLOAD);
    header = valid_header();
    CHECK(wd_client_video_packet_validate(&header, sizeof(header) + header.data_size - 1, &expected, &control) ==
          WD_CLIENT_VIDEO_PACKET_INVALID_PAYLOAD);
}

void test_control_frames_may_carry_transition_geometry() {
    const wd_client_video_packet_expectation expected{7, 0x1122334455667788ull, 800, 600};
    auto header = valid_header();
    header.flags = WD_VIDEO_FRAME_RESIZE | WD_VIDEO_FRAME_END_OF_STREAM;
    header.width = 1366;
    header.height = 717;
    header.coded_width = 1366;
    header.coded_height = 717;
    header.frame_id = 0;
    header.data_size = 0;
    bool control = false;
    CHECK(wd_client_video_packet_validate(&header, sizeof(header), &expected, &control) == WD_CLIENT_VIDEO_PACKET_VALID);
    CHECK(control);

    header.flags = 0;
    CHECK(wd_client_video_packet_validate(&header, sizeof(header), &expected, &control) ==
          WD_CLIENT_VIDEO_PACKET_INVALID_PAYLOAD);
}

} // namespace

int main() {
    test_payload_identity_and_geometry();
    test_control_frames_may_carry_transition_geometry();
    return 0;
}
