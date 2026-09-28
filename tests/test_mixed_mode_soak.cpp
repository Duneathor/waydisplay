#include "audio_video_sync.h"
#include "stream_ownership.h"
#include "video_decode_queue_policy.h"
#include "window_render_policy.hpp"
#include "wd_connection_identity.h"
#include "wd_video_transition.h"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_video_offer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {

struct DecodeModel {
    uint32_t depth = 0;
    bool waiting_keyframe = false;
    uint64_t keyframe_requests = 0;
    uint64_t decoded = 0;
    uint64_t presented = 0;

    void overload_and_recover() {
        constexpr uint32_t capacity = WD_CLIENT_VIDEO_DECODE_INPUT_QUEUE_CAPACITY;
        for (uint32_t i = 0; i < capacity; ++i)
        {
            const auto plan = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting_keyframe, false, false);
            CHECK(plan.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
            ++depth;
        }
        auto plan = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting_keyframe, false, false);
        CHECK(plan.action == WD_CLIENT_VIDEO_DECODE_QUEUE_RECOVER_OVERFLOW);
        CHECK(plan.clear_queue && plan.wait_for_keyframe);
        depth = 0;
        waiting_keyframe = true;
        ++keyframe_requests;

        for (unsigned i = 0; i < 32; ++i)
        {
            plan = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting_keyframe, false, false);
            CHECK(plan.action == WD_CLIENT_VIDEO_DECODE_QUEUE_DROP_UNTIL_KEYFRAME);
            CHECK(plan.wait_for_keyframe);
        }

        plan = wd_client_video_decode_queue_plan_compute(depth, capacity, waiting_keyframe, true, false);
        CHECK(plan.action == WD_CLIENT_VIDEO_DECODE_QUEUE_ENQUEUE);
        CHECK(plan.reset_decoder_before && !plan.wait_for_keyframe);
        waiting_keyframe = false;
        depth = 1;
        decoded += depth;
        presented += depth;
        depth = 0;
    }
};

struct MixedModeModel {
    uint8_t session_id = 0;
    uint64_t connection_epoch = 0;
    uint64_t content_epoch = 0;
    uint64_t bootstrap_epoch = 0;
    uint64_t presented_tile_epoch = 0;
    bool connected = false;
    bool visible = true;
    bool focused = true;
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;
    DecodeModel decode;

    void connect() {
        connected = true;
        session_id = wd_connection_next_session_id(session_id);
        connection_epoch = wd_next_nonzero_epoch(connection_epoch);
        content_epoch = wd_next_nonzero_epoch(content_epoch);
        bootstrap_epoch = content_epoch;
        presented_tile_epoch = 0;
        wd_client_stream_ownership_reset_to_tiles(&ownership);
        CHECK(wd_client_stream_ownership_snapshot(&ownership).owner == WD_CLIENT_CONTENT_OWNER_TILES);
    }

    void present_bootstrap() {
        CHECK(bootstrap_epoch == content_epoch);
        presented_tile_epoch = content_epoch;
        bootstrap_epoch = 0;
    }

    uint64_t enter_video() {
        CHECK(connected && bootstrap_epoch == 0);
        const auto plan = wd_video_entry_plan_make(content_epoch, true, true);
        CHECK(wd_video_entry_plan_can_commit(&plan, content_epoch, true));
        content_epoch = plan.frame_content_epoch;
        const uint64_t local_epoch = wd_client_stream_ownership_begin_video_stream(&ownership);
        CHECK(wd_client_stream_ownership_is_current(&ownership, local_epoch, WD_CLIENT_CONTENT_OWNER_VIDEO));
        return local_epoch;
    }

    void focus_cycle() {
        const auto before = wd_client_stream_ownership_snapshot(&ownership);
        focused = waydisplay::client_window_focus_after_event(focused, waydisplay::ClientWindowFocusChange::Lost);
        CHECK((waydisplay::client_window_feedback_flags(visible, focused) & WD_CLIENT_STATS_WINDOW_FOCUSED) == 0);
        focused = waydisplay::client_window_focus_after_event(focused, waydisplay::ClientWindowFocusChange::Gained);
        CHECK((waydisplay::client_window_feedback_flags(visible, focused) & WD_CLIENT_STATS_WINDOW_FOCUSED) != 0);
        const auto after = wd_client_stream_ownership_snapshot(&ownership);
        CHECK(before.epoch == after.epoch && before.owner == after.owner);
    }

    void planned_resize_recovery(uint64_t stale_video_local_epoch) {
        content_epoch = wd_next_nonzero_epoch(content_epoch);
        const uint64_t required = content_epoch;
        const uint64_t local_tiles = wd_client_stream_ownership_end_video_stream(&ownership);
        CHECK(!wd_client_stream_ownership_is_current(&ownership, stale_video_local_epoch, WD_CLIENT_CONTENT_OWNER_VIDEO));
        CHECK(wd_client_stream_ownership_is_current(&ownership, local_tiles, WD_CLIENT_CONTENT_OWNER_TILES));
        CHECK(wd_tile_recovery_decide(true, required, required - 1, 1, 5) == WD_TILE_RECOVERY_WAIT);
        CHECK(wd_tile_recovery_decide(true, required, required, 1, 5) == WD_TILE_RECOVERY_COMPLETE_PRESENTED);
        presented_tile_epoch = required;
    }

    void disconnect() {
        connected = false;
        wd_client_stream_ownership_reset_to_tiles(&ownership);
    }
};

void alternating_session_mode_soak() {
    struct SessionModeCase {
        uint8_t video_mode;
        uint8_t decoder_mode;
        bool expect_video;
    };
    const SessionModeCase cases[] = {
        {WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_AUTO, true},
        {WD_VIDEO_MODE_OFF, WD_CLIENT_VIDEO_DECODER_AUTO, false},
        {WD_VIDEO_MODE_AUTO, WD_CLIENT_VIDEO_DECODER_OFF, false},
        {WD_VIDEO_MODE_FORCE, WD_CLIENT_VIDEO_DECODER_AUTO, true},
    };

    uint8_t session_id = 0;
    uint64_t connection_epoch = 0;
    uint64_t content_epoch = 0;
    uint64_t previous_video_local_epoch = 0;
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;

    constexpr unsigned AlternatingCycles = 1024;
    for (unsigned cycle = 0; cycle < AlternatingCycles; ++cycle)
    {
        const SessionModeCase& test = cases[cycle % (sizeof(cases) / sizeof(cases[0]))];
        const wd_client_video_offer offer = wd_client_video_offer_decide(
            test.video_mode, test.decoder_mode, WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265,
            WD_VIDEO_CODEC_H265);
        const bool negotiated = (offer.capabilities & WD_CLIENT_CAP_VIDEO_STREAM) != 0;
        CHECK(negotiated == test.expect_video);
        CHECK(wd_video_session_bootstrap_required(test.video_mode, offer.capabilities) == test.expect_video);

        const uint8_t old_session_id = session_id;
        const uint64_t old_connection_epoch = connection_epoch;
        session_id = wd_connection_next_session_id(session_id);
        connection_epoch = wd_next_nonzero_epoch(connection_epoch);
        content_epoch = wd_next_nonzero_epoch(content_epoch);
        CHECK(session_id != 0 && session_id != old_session_id);
        CHECK(connection_epoch > old_connection_epoch);

        wd_client_stream_ownership_reset_to_tiles(&ownership);
        const auto tile_snapshot = wd_client_stream_ownership_snapshot(&ownership);
        CHECK(tile_snapshot.owner == WD_CLIENT_CONTENT_OWNER_TILES);
        if (previous_video_local_epoch != 0)
        {
            CHECK(!wd_client_stream_ownership_is_current(&ownership, previous_video_local_epoch,
                                                         WD_CLIENT_CONTENT_OWNER_VIDEO));
        }

        if (!test.expect_video)
        {
            CHECK(offer.codecs == 0 && offer.transport == 0);
            CHECK(!wd_video_control_allows_entry(test.video_mode, negotiated, false, true));
            CHECK(wd_client_stream_ownership_snapshot(&ownership).owner == WD_CLIENT_CONTENT_OWNER_TILES);
            previous_video_local_epoch = 0;
            continue;
        }

        /* The reconnect starts with a fresh bootstrap fence. A stale tile
         * presentation from the previous session cannot open video entry. */
        CHECK(!wd_video_entry_allowed(true, false, 0, test.video_mode == WD_VIDEO_MODE_FORCE,
                                      WD_VIDEO_RECOVERY_NONE));
        if (content_epoch > 1)
        {
            CHECK(wd_tile_recovery_decide(true, content_epoch, content_epoch - 1, 0, 5) == WD_TILE_RECOVERY_WAIT);
        }
        CHECK(wd_tile_recovery_decide(true, content_epoch, content_epoch, 0, 5) ==
              WD_TILE_RECOVERY_COMPLETE_PRESENTED);
        CHECK(wd_video_control_allows_entry(test.video_mode, negotiated, true, true));
        CHECK(wd_video_entry_allowed(false, false, 0, test.video_mode == WD_VIDEO_MODE_FORCE,
                                     WD_VIDEO_RECOVERY_NONE));

        previous_video_local_epoch = wd_client_stream_ownership_begin_video_stream(&ownership);
        CHECK(wd_client_stream_ownership_is_current(&ownership, previous_video_local_epoch,
                                                    WD_CLIENT_CONTENT_OWNER_VIDEO));
        (void)wd_client_stream_ownership_end_video_stream(&ownership);
    }
}

} // namespace

int main() {
    alternating_session_mode_soak();

    MixedModeModel model;
    uint64_t last_connection_epoch = 0;
    uint64_t last_content_epoch = 0;
    uint64_t total_recoveries = 0;

    constexpr unsigned Cycles = 512;
    for (unsigned cycle = 0; cycle < Cycles; ++cycle)
    {
        model.connect();
        CHECK(model.connection_epoch > last_connection_epoch);
        CHECK(model.content_epoch > last_content_epoch);
        last_connection_epoch = model.connection_epoch;
        last_content_epoch = model.content_epoch;

        /* A stale presentation from the previous cycle cannot complete bootstrap. */
        if (model.content_epoch > 1)
        {
            CHECK(wd_tile_recovery_decide(true, model.content_epoch, model.content_epoch - 1, 0, 5) == WD_TILE_RECOVERY_WAIT);
        }
        model.present_bootstrap();
        CHECK(wd_video_entry_allowed(false, false, 0, false, WD_VIDEO_RECOVERY_NONE));

        const uint64_t video_local_epoch = model.enter_video();
        model.focus_cycle();

        if ((cycle % 3u) == 0)
        {
            const uint64_t before_requests = model.decode.keyframe_requests;
            model.decode.overload_and_recover();
            CHECK(model.decode.keyframe_requests == before_requests + 1);
            ++total_recoveries;
        }

        CHECK(wd_client_audio_startup_gate_decide(true, false, true, 1000, 1000) == WD_CLIENT_AUDIO_STARTUP_TIMEOUT);
        model.planned_resize_recovery(video_local_epoch);
        last_content_epoch = model.content_epoch;

        const uint64_t second_video_epoch = model.enter_video();
        CHECK(second_video_epoch > video_local_epoch);
        model.planned_resize_recovery(second_video_epoch);
        last_content_epoch = model.content_epoch;

        model.disconnect();
        CHECK(!model.connected);
    }

    CHECK(total_recoveries != 0);
    CHECK(model.decode.decoded == total_recoveries);
    CHECK(model.decode.presented == total_recoveries);
    CHECK(model.decode.keyframe_requests == total_recoveries);
    std::puts("mixed mode soak: PASS");
    return 0;
}
