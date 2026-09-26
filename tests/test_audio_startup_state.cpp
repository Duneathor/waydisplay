#include "audio_startup_state.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_configured_silence_does_not_wait() {
    ClientAudioStartupGateState state{};
    client_audio_startup_gate_reset(state);
    require(!client_audio_startup_gate_waiting(state),
            "configuration without PCM must leave video ungated");
    require(state.phase == ClientAudioStartupGatePhase::Idle,
            "configured silence should remain eligible for the first PCM transition");
}

void test_first_pcm_arms_one_bounded_wait() {
    ClientAudioStartupGateState state{};
    client_audio_startup_gate_begin_buffering(state, UINT64_C(2500000000));
    require(client_audio_startup_gate_waiting(state),
            "the first queued PCM must arm startup synchronization");
    require(state.wait_started_ns == UINT64_C(2500000000),
            "the first PCM packet must establish the wait origin");

    client_audio_startup_gate_begin_buffering(state, UINT64_C(2600000000));
    require(state.wait_started_ns == UINT64_C(2500000000),
            "later PCM packets must not extend the bounded startup window");
    require(client_audio_startup_gate_elapsed_ms(state, UINT64_C(3499000000)) == 999,
            "elapsed wait time must use the first queued PCM timestamp");
}

void test_timeout_release_is_sticky_for_current_buffering_period() {
    ClientAudioStartupGateState state{};
    client_audio_startup_gate_begin_buffering(state, UINT64_C(1000000));
    client_audio_startup_gate_release(state);
    require(!client_audio_startup_gate_waiting(state),
            "a timeout must release the startup gate");

    client_audio_startup_gate_begin_buffering(state, UINT64_C(2000000));
    require(state.phase == ClientAudioStartupGatePhase::Released,
            "more PCM must not re-arm a wait that already timed out");
}

void test_new_buffering_period_can_arm_after_reset() {
    ClientAudioStartupGateState state{};
    client_audio_startup_gate_begin_buffering(state, UINT64_C(1000000));
    client_audio_startup_gate_release(state);
    client_audio_startup_gate_reset(state);
    client_audio_startup_gate_begin_buffering(state, UINT64_C(9000000));

    require(client_audio_startup_gate_waiting(state),
            "an underflow or discontinuity may start a new bounded wait");
    require(state.wait_started_ns == UINT64_C(9000000),
            "a new buffering period needs its own wait origin");
}

void test_output_rebase_can_relinquish_clock_without_rearming() {
    ClientAudioStartupGateState state{};
    client_audio_startup_gate_begin_buffering(state, UINT64_C(1000000));
    client_audio_startup_gate_release(state);

    client_audio_startup_gate_begin_buffering(state, UINT64_C(5000000));
    require(!client_audio_startup_gate_waiting(state),
            "output-only rebuffering must remain free-running after clock release");
}

} // namespace

int main() {
    test_configured_silence_does_not_wait();
    test_first_pcm_arms_one_bounded_wait();
    test_timeout_release_is_sticky_for_current_buffering_period();
    test_new_buffering_period_can_arm_after_reset();
    test_output_rebase_can_relinquish_clock_without_rearming();
    return EXIT_SUCCESS;
}
