#include "udp_transport_lifecycle.hpp"
#include "video_transition.hpp"
#include "wd_connection_identity.h"
#include "wd_input_correlation.h"
#include "wd_video_transition.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct RandomScript {
    std::vector<std::uint8_t> bytes;
    std::size_t               offset         = 0;
    std::size_t               max_chunk      = SIZE_MAX;
    unsigned                  calls          = 0;
    unsigned                  interrupt_call = 0;
    unsigned                  fail_call      = 0;
};

ssize_t scripted_random_read(void* buffer, std::size_t size, void* user_data) {
    auto& script = *static_cast<RandomScript*>(user_data);
    script.calls++;
    if (script.calls == script.interrupt_call)
    {
        errno = EINTR;
        return -1;
    }
    if (script.calls == script.fail_call)
    {
        errno = EIO;
        return -1;
    }
    if (script.offset >= script.bytes.size())
    {
        return 0;
    }
    const std::size_t available = script.bytes.size() - script.offset;
    const std::size_t amount    = std::min({size, available, script.max_chunk});
    std::memcpy(buffer, script.bytes.data() + script.offset, amount);
    script.offset += amount;
    return static_cast<ssize_t>(amount);
}

void append_u64(RandomScript& script, std::uint64_t value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    script.bytes.insert(script.bytes.end(), begin, begin + sizeof(value));
}

void test_connection_identity_requires_secure_randomness() {
    RandomScript partial;
    partial.max_chunk      = 3;
    partial.interrupt_call = 2;
    append_u64(partial, UINT64_C(0x1122334455667788));
    append_u64(partial, UINT64_C(0x8877665544332211));
    std::uint64_t token    = 0;
    std::uint64_t clock_id = 0;
    require(wd_connection_identity_generate_with(scripted_random_read, &partial, &token, &clock_id),
            "short secure-random reads and EINTR should be retried");
    require(token == UINT64_C(0x1122334455667788) && clock_id == UINT64_C(0x8877665544332211),
            "secure random identities should be preserved exactly");

    RandomScript zero_then_valid;
    append_u64(zero_then_valid, 0);
    append_u64(zero_then_valid, 17);
    append_u64(zero_then_valid, 17);
    append_u64(zero_then_valid, 18);
    require(wd_connection_identity_generate_with(scripted_random_read, &zero_then_valid, &token, &clock_id),
            "zero and duplicate values should be retried");
    require(token == 17 && clock_id == 18, "connection and media identities must be non-zero and distinct");

    RandomScript failed;
    failed.fail_call = 1;
    token            = 41;
    clock_id         = 42;
    require(!wd_connection_identity_generate_with(scripted_random_read, &failed, &token, &clock_id),
            "random-source failure must reject the session identity");
    require(token == 41 && clock_id == 42, "failed identity generation must not publish partial state");

    RandomScript zeros;
    for (unsigned i = 0; i < 8; ++i)
    {
        append_u64(zeros, 0);
    }
    require(!wd_connection_identity_generate_with(scripted_random_read, &zeros, &token, &clock_id),
            "a random source stuck at zero must fail closed");
}

void test_first_keyframe_reserves_next_epoch_without_early_commit() {
    const wd_video_entry_plan plan = wd_video_entry_plan_make(9, true, true);
    require(plan.source_content_epoch == 9, "entry plan should retain source epoch");
    require(plan.frame_content_epoch == 10, "first keyframe should carry next epoch");
    require(plan.commit_on_queue, "first keyframe should commit ownership on queue success");
    require(!wd_video_entry_plan_can_commit(&plan, 9, false), "queue failure must not advance content ownership");
    require(wd_video_entry_plan_can_commit(&plan, 9, true), "successful first-keyframe queue should commit the reserved epoch");
    require(!wd_video_entry_plan_can_commit(&plan, 10, true), "stale plans cannot commit after another transition");
}

void test_nonentry_frames_keep_current_epoch() {
    const wd_video_entry_plan active = wd_video_entry_plan_make(21, false, false);
    require(active.frame_content_epoch == 21 && !active.commit_on_queue, "active video frames should remain in the current epoch");

    const wd_video_entry_plan non_keyframe = wd_video_entry_plan_make(UINT64_MAX, true, false);
    require(non_keyframe.frame_content_epoch == UINT64_MAX && !non_keyframe.commit_on_queue, "video entry cannot commit on a non-keyframe");

    const wd_video_entry_plan wrapped = wd_video_entry_plan_make(UINT64_MAX, true, true);
    require(wrapped.frame_content_epoch == 1, "content epoch must skip zero on wrap");
}

void test_udp_fallback_never_reuses_a_socket_still_owned_by_io_uring() {
    using namespace waydisplay;
    require(client_udp_fallback_action(ClientAsyncUdpDetachResult::Detached, true) == ClientUdpFallbackAction::ReuseSocket,
            "fully detached receiver may reuse its socket");
    require(client_udp_fallback_action(ClientAsyncUdpDetachResult::SocketStillOwned, true) == ClientUdpFallbackAction::ReplaceSocket,
            "outstanding receives require a replacement socket");
    require(client_udp_fallback_action(ClientAsyncUdpDetachResult::Detached, false) == ClientUdpFallbackAction::Abort,
            "fallback cannot proceed without a socket");
}

void test_input_correlation_commits_only_matching_successful_delivery() {
    require(wd_input_correlation_select(true, 44, 0) == 44, "pending input should be selected when no delivery is in flight");
    require(wd_input_correlation_select(true, 45, 44) == 0, "a second correlation must wait for the first delivery");

    wd_input_correlation_completion failed = wd_input_correlation_complete(44, 44, 44, false);
    require(failed.matched_inflight && !failed.clear_pending, "failed delivery should release inflight state without consuming input");

    wd_input_correlation_completion superseded = wd_input_correlation_complete(44, 45, 44, true);
    require(superseded.matched_inflight && !superseded.clear_pending, "newer input should remain pending after an older delivery succeeds");

    wd_input_correlation_completion success = wd_input_correlation_complete(45, 45, 45, true);
    require(success.matched_inflight && success.clear_pending, "matching successful delivery should consume the pending input");

    wd_input_correlation_completion stale = wd_input_correlation_complete(46, 46, 45, true);
    require(!stale.matched_inflight && !stale.clear_pending, "stale completion cannot alter current correlation state");
}

void test_client_video_transition_state_machine() {
    using namespace waydisplay;

    ClientVideoTransitionDecision decision = client_video_transition(ClientVideoPhase::Tiles, true, false, false, false, true);
    require(!decision.accept_payload && decision.next_phase == ClientVideoPhase::AwaitingKeyframe,
            "a video epoch cannot begin on a non-keyframe");
    require(decision.reset_decoder, "a new video epoch should reset a tile-owned decoder");

    decision = client_video_transition(ClientVideoPhase::AwaitingKeyframe, true, false, false, true, true);
    require(decision.accept_payload && decision.next_phase == ClientVideoPhase::Video,
            "the first matching keyframe should enter video ownership");
    require(!decision.reset_decoder, "a config reset followed by the same transition keyframe must not reset twice");

    decision = client_video_transition(ClientVideoPhase::Video, false, true, true, false, false);
    require(!decision.accept_payload && decision.next_phase == ClientVideoPhase::AwaitingKeyframe,
            "resize EOS should wait for the next configured keyframe");
    require(decision.reset_decoder, "resize should invalidate an active decoder once");

    decision = client_video_transition(ClientVideoPhase::AwaitingKeyframe, false, false, true, false, false);
    require(!decision.reset_decoder, "a duplicate resize control must not tear down an already-reset decoder");
}

} // namespace

static void test_connection_session_rotation() {
    require(wd_connection_next_session_id(0) == 1, "first connection session must be non-zero");
    require(wd_connection_next_session_id(1) == 2, "reconnect must advance the session ID");
    require(wd_connection_next_session_id(254) == 255, "session increment before wrap");
    require(wd_connection_next_session_id(255) == 1, "session wrap must skip zero");

    uint8_t session = 1;
    for (unsigned i = 0; i < 1024; ++i)
    {
        const uint8_t previous = session;
        session                = wd_connection_next_session_id(session);
        require(session != 0, "rotated session must never be zero");
        require(session != previous, "rotated session must differ from previous connection");
    }
}
int main() {
    test_connection_session_rotation();
    test_connection_identity_requires_secure_randomness();
    test_first_keyframe_reserves_next_epoch_without_early_commit();
    test_nonentry_frames_keep_current_epoch();
    test_udp_fallback_never_reuses_a_socket_still_owned_by_io_uring();
    test_input_correlation_commits_only_matching_successful_delivery();
    test_client_video_transition_state_machine();
    return 0;
}
