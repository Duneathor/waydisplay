#include "video_decode_queue_policy.h"
#include "waydisplay/wd_config.h"

#include <cstdint>
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

struct MockVideoPipeline {
    uint32_t depth = 0;
    bool waiting_for_keyframe = false;
    uint64_t network_messages = 0;
    uint64_t dropped = 0;
    uint64_t keyframe_requests = 0;
    uint64_t decoder_resets = 0;
    uint64_t decoded = 0;
    uint64_t presented = 0;

    bool receive(bool keyframe, bool control = false) {
        ++network_messages;
        const auto plan = wd_client_video_decode_queue_plan_compute(
            depth, WD_CLIENT_VIDEO_DECODE_INPUT_QUEUE_CAPACITY,
            waiting_for_keyframe, keyframe, control);

        if (plan.clear_queue) depth = 0;
        waiting_for_keyframe = plan.wait_for_keyframe;

        switch (plan.action)
        {
        case WD_CLIENT_VIDEO_DECODE_QUEUE_DROP_UNTIL_KEYFRAME:
            ++dropped;
            return true;
        case WD_CLIENT_VIDEO_DECODE_QUEUE_RECOVER_OVERFLOW:
            ++dropped;
            ++keyframe_requests;
            return true;
        case WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE:
            if (plan.reset_decoder_before) ++decoder_resets;
            ++depth;
            return true;
        }
        return false;
    }

    void decode_all() {
        decoded += depth;
        presented += depth;
        depth = 0;
    }
};

int main() {
    MockVideoPipeline pipeline;
    constexpr uint32_t capacity = WD_CLIENT_VIDEO_DECODE_INPUT_QUEUE_CAPACITY;

    /* Stall the decoder while the network side keeps receiving. */
    for (uint32_t i = 0; i < capacity; ++i) CHECK(pipeline.receive(false));
    CHECK(pipeline.depth == capacity);

    CHECK(pipeline.receive(false));
    CHECK(pipeline.depth == 0);
    CHECK(pipeline.waiting_for_keyframe);
    CHECK(pipeline.keyframe_requests == 1);

    /* Subsequent dependent frames are consumed by the network path and
     * dropped locally. They must not generate a keyframe-request storm. */
    for (uint32_t i = 0; i < 256; ++i) CHECK(pipeline.receive(false));
    CHECK(pipeline.network_messages == static_cast<uint64_t>(capacity) + 257u);
    CHECK(pipeline.dropped == 257u);
    CHECK(pipeline.keyframe_requests == 1);
    CHECK(pipeline.depth == 0);

    /* A recovery keyframe scrubs decoder references and reopens normal queueing. */
    CHECK(pipeline.receive(true));
    CHECK(!pipeline.waiting_for_keyframe);
    CHECK(pipeline.decoder_resets == 1);
    CHECK(pipeline.depth == 1);
    for (uint32_t i = 1; i < capacity; ++i) CHECK(pipeline.receive(false));
    CHECK(pipeline.depth == capacity);

    pipeline.decode_all();
    CHECK(pipeline.decoded == capacity);
    CHECK(pipeline.presented == capacity);
    CHECK(pipeline.depth == 0);

    /* A later overload is a new recovery episode and may request one new keyframe. */
    for (uint32_t i = 0; i < capacity; ++i) CHECK(pipeline.receive(false));
    CHECK(pipeline.receive(false));
    CHECK(pipeline.keyframe_requests == 2);
    CHECK(pipeline.waiting_for_keyframe);

    std::puts("video decode overflow recovery: PASS");
    return 0;
}
