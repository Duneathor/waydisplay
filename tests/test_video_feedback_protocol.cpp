#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_dispatch.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    CHECK((WD_CLIENT_CAP_MASK & WD_CLIENT_CAP_VIDEO_FEEDBACK) != 0);
    CHECK((WD_SERVER_CAP_MASK & WD_SERVER_CAP_VIDEO_FEEDBACK) != 0);
    CHECK(sizeof(wd_video_feedback_payload) == 73);
    CHECK(wd_protocol_message_allowed(WD_MSG_VIDEO_FEEDBACK, WD_PROTOCOL_CHANNEL_CONTROL,
                                      WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
                                      sizeof(wd_video_feedback_payload)));
    CHECK(!wd_protocol_message_allowed(WD_MSG_VIDEO_FEEDBACK, WD_PROTOCOL_CHANNEL_CONTROL,
                                       WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
                                       sizeof(wd_video_feedback_payload)));
    CHECK(!wd_protocol_message_allowed(WD_MSG_VIDEO_FEEDBACK, WD_PROTOCOL_CHANNEL_CONTROL,
                                       WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
                                       sizeof(wd_video_feedback_payload)));
    wd_video_feedback_payload feedback{};
    feedback.flags = WD_VIDEO_FEEDBACK_NEEDS_KEYFRAME | WD_VIDEO_FEEDBACK_DECODE_OVERLOAD;
    CHECK((feedback.flags & ~WD_VIDEO_FEEDBACK_FLAG_MASK) == 0);
    return 0;
}
