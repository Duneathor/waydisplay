#include "udp_transport_lifecycle.hpp"
#include "wd_video_transition.h"
#include "wd_input_correlation.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_first_keyframe_reserves_next_epoch_without_early_commit() {
    const wd_video_entry_plan plan = wd_video_entry_plan_make(9, true, true);
    require(plan.source_content_epoch == 9, "entry plan should retain source epoch");
    require(plan.frame_content_epoch == 10, "first keyframe should carry next epoch");
    require(plan.commit_on_queue, "first keyframe should commit ownership on queue success");
    require(!wd_video_entry_plan_can_commit(&plan, 9, false),
            "queue failure must not advance content ownership");
    require(wd_video_entry_plan_can_commit(&plan, 9, true),
            "successful first-keyframe queue should commit the reserved epoch");
    require(!wd_video_entry_plan_can_commit(&plan, 10, true),
            "stale plans cannot commit after another transition");
}

void test_nonentry_frames_keep_current_epoch() {
    const wd_video_entry_plan active = wd_video_entry_plan_make(21, false, false);
    require(active.frame_content_epoch == 21 && !active.commit_on_queue,
            "active video frames should remain in the current epoch");

    const wd_video_entry_plan non_keyframe = wd_video_entry_plan_make(UINT64_MAX, true, false);
    require(non_keyframe.frame_content_epoch == UINT64_MAX && !non_keyframe.commit_on_queue,
            "video entry cannot commit on a non-keyframe");

    const wd_video_entry_plan wrapped = wd_video_entry_plan_make(UINT64_MAX, true, true);
    require(wrapped.frame_content_epoch == 1, "content epoch must skip zero on wrap");
}

void test_udp_fallback_never_reuses_a_socket_still_owned_by_io_uring() {
    using namespace waydisplay;
    require(client_udp_fallback_action(ClientAsyncUdpDetachResult::Detached, true) ==
                ClientUdpFallbackAction::ReuseSocket,
            "fully detached receiver may reuse its socket");
    require(client_udp_fallback_action(ClientAsyncUdpDetachResult::SocketStillOwned, true) ==
                ClientUdpFallbackAction::ReplaceSocket,
            "outstanding receives require a replacement socket");
    require(client_udp_fallback_action(ClientAsyncUdpDetachResult::Detached, false) ==
                ClientUdpFallbackAction::Abort,
            "fallback cannot proceed without a socket");
}

void test_input_correlation_commits_only_matching_successful_delivery() {
    require(wd_input_correlation_select(true, 44, 0) == 44,
            "pending input should be selected when no delivery is in flight");
    require(wd_input_correlation_select(true, 45, 44) == 0,
            "a second correlation must wait for the first delivery");

    wd_input_correlation_completion failed = wd_input_correlation_complete(44, 44, 44, false);
    require(failed.matched_inflight && !failed.clear_pending,
            "failed delivery should release inflight state without consuming input");

    wd_input_correlation_completion superseded = wd_input_correlation_complete(44, 45, 44, true);
    require(superseded.matched_inflight && !superseded.clear_pending,
            "newer input should remain pending after an older delivery succeeds");

    wd_input_correlation_completion success = wd_input_correlation_complete(45, 45, 45, true);
    require(success.matched_inflight && success.clear_pending,
            "matching successful delivery should consume the pending input");

    wd_input_correlation_completion stale = wd_input_correlation_complete(46, 46, 45, true);
    require(!stale.matched_inflight && !stale.clear_pending,
            "stale completion cannot alter current correlation state");
}

} // namespace

int main() {
    test_first_keyframe_reserves_next_epoch_without_early_commit();
    test_nonentry_frames_keep_current_epoch();
    test_udp_fallback_never_reuses_a_socket_still_owned_by_io_uring();
    test_input_correlation_commits_only_matching_successful_delivery();
    return 0;
}
