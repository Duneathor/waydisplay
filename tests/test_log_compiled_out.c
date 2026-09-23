/* Override the project-wide -D level for this test without a duplicate -D. */
#undef WAYDISPLAY_LOG_LEVEL
#define WAYDISPLAY_LOG_LEVEL 2
#include "waydisplay/wd_log.h"

int wd_test_log_compiled_out_c(void);

static int log_only_c(int* calls) {
    ++*calls;
    return 9;
}

int wd_test_log_compiled_out_c(void) {
    int calls = 0;
    const int stats_only = 21;
    const int debug_only = 42;
    WD_LOG_STATS("stats=%d, function=%d", stats_only, log_only_c(&calls));
    WD_LOG_DEBUG("debug=%d, function=%d", debug_only, log_only_c(&calls));
    return calls == 0;
}
