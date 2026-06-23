#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_input_correlation_completion {
    bool matched_inflight;
    bool clear_pending;
};

uint64_t wd_input_correlation_select(bool input_pending, uint64_t latest_sequence, uint64_t inflight_sequence);
struct wd_input_correlation_completion wd_input_correlation_complete(uint64_t inflight_sequence, uint64_t latest_sequence,
                                                                     uint64_t completed_sequence, bool success);

#ifdef __cplusplus
}
#endif
