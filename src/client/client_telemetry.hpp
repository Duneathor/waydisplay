#pragma once

#include "client_state.hpp"

#include <atomic>
#include <cstdint>

namespace waydisplay {

void record_atomic_max(std::atomic<uint64_t>& value, uint64_t sample);
bool take_input_timestamp(ClientState& state, uint64_t sequence, uint64_t& timestamp_ns);
void sample_client_stats(ClientState& state, bool log_stats);

} // namespace waydisplay
