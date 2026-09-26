#pragma once

#include <cstdint>

namespace waydisplay {

enum class ClientAudioStartupGatePhase : uint8_t {
    Idle = 0,
    Waiting,
    Released,
};

struct ClientAudioStartupGateState {
    ClientAudioStartupGatePhase phase = ClientAudioStartupGatePhase::Idle;
    uint64_t                    wait_started_ns = 0;
};

inline void client_audio_startup_gate_reset(ClientAudioStartupGateState& state) {
    state.phase = ClientAudioStartupGatePhase::Idle;
    state.wait_started_ns = 0;
}

inline void client_audio_startup_gate_begin_buffering(ClientAudioStartupGateState& state, uint64_t now_ns) {
    if (state.phase != ClientAudioStartupGatePhase::Idle)
    {
        return;
    }
    state.phase = ClientAudioStartupGatePhase::Waiting;
    state.wait_started_ns = now_ns;
}

inline void client_audio_startup_gate_release(ClientAudioStartupGateState& state) {
    state.phase = ClientAudioStartupGatePhase::Released;
    state.wait_started_ns = 0;
}

inline bool client_audio_startup_gate_waiting(const ClientAudioStartupGateState& state) {
    return state.phase == ClientAudioStartupGatePhase::Waiting;
}

inline uint64_t client_audio_startup_gate_elapsed_ms(const ClientAudioStartupGateState& state, uint64_t now_ns) {
    if (!client_audio_startup_gate_waiting(state) || state.wait_started_ns == 0 || now_ns <= state.wait_started_ns)
    {
        return 0;
    }
    return (now_ns - state.wait_started_ns) / UINT64_C(1000000);
}

} // namespace waydisplay
