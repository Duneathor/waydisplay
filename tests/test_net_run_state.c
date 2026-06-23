#include "wd_net_run_state.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            exit(1);                                                                                                                       \
        }                                                                                                                                  \
    } while (0)

struct reader_context {
    const struct wd_net_run_state* state;
    atomic_uint*                   ready_count;
};

static void* reader_main(void* opaque) {
    struct reader_context* context = opaque;
    atomic_fetch_add_explicit(context->ready_count, 1u, memory_order_release);

    while (wd_net_run_state_is_running(context->state))
    {
        sched_yield();
    }

    return NULL;
}

int main(void) {
    struct wd_net_run_state state;
    wd_net_run_state_init(&state, true);
    CHECK(wd_net_run_state_is_running(&state));

    enum { READER_COUNT = 4 };
    pthread_t   threads[READER_COUNT];
    atomic_uint ready_count;
    atomic_init(&ready_count, 0u);

    struct reader_context context = {
        .state       = &state,
        .ready_count = &ready_count,
    };

    for (size_t i = 0; i < READER_COUNT; ++i)
    {
        CHECK(pthread_create(&threads[i], NULL, reader_main, &context) == 0);
    }

    while (atomic_load_explicit(&ready_count, memory_order_acquire) != READER_COUNT)
    {
        sched_yield();
    }

    CHECK(wd_net_run_state_stop(&state));
    CHECK(!wd_net_run_state_is_running(&state));
    CHECK(!wd_net_run_state_stop(&state));

    for (size_t i = 0; i < READER_COUNT; ++i)
    {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }

    wd_net_run_state_set(&state, true);
    CHECK(wd_net_run_state_is_running(&state));
    return 0;
}
