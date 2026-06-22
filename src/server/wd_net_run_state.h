#pragma once

#include <stdatomic.h>
#include <stdbool.h>

struct wd_net_run_state {
    atomic_bool running;
};

static inline void wd_net_run_state_init(struct wd_net_run_state* state, bool running) {
    atomic_init(&state->running, running);
}

static inline bool wd_net_run_state_is_running(const struct wd_net_run_state* state) {
    return atomic_load_explicit(&state->running, memory_order_acquire);
}

static inline void wd_net_run_state_set(struct wd_net_run_state* state, bool running) {
    atomic_store_explicit(&state->running, running, memory_order_release);
}

static inline bool wd_net_run_state_stop(struct wd_net_run_state* state) {
    return atomic_exchange_explicit(&state->running, false, memory_order_acq_rel);
}
