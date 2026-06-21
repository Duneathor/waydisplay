#include "wd_input_correlation.h"

uint64_t wd_input_correlation_select(bool input_pending, uint64_t latest_sequence,
                                     uint64_t inflight_sequence) {
    return input_pending && latest_sequence != 0 && inflight_sequence == 0 ? latest_sequence : 0;
}

struct wd_input_correlation_completion wd_input_correlation_complete(uint64_t inflight_sequence,
                                                                     uint64_t latest_sequence,
                                                                     uint64_t completed_sequence,
                                                                     bool success) {
    struct wd_input_correlation_completion result = {0};
    result.matched_inflight = completed_sequence != 0 && completed_sequence == inflight_sequence;
    result.clear_pending = result.matched_inflight && success && latest_sequence == completed_sequence;
    return result;
}
