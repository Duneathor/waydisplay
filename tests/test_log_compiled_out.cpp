/* Override the project-wide -D level for this test without a duplicate -D. */
#undef WAYDISPLAY_LOG_LEVEL
#define WAYDISPLAY_LOG_LEVEL 2
#include "waydisplay/wd_log.h"

extern "C" int wd_test_log_compiled_out_c(void);

namespace {
int log_only_cpp(int& calls) {
    ++calls;
    return 7;
}
} // namespace

int main() {
    int calls = 0;
    const int stats_only = 21;
    const int debug_only = 42;
    WD_LOG_STATS("stats=%d, function=%d", stats_only, log_only_cpp(calls));
    WD_LOG_DEBUG("debug=%d, function=%d", debug_only, log_only_cpp(calls));
    return calls == 0 && wd_test_log_compiled_out_c() ? 0 : 1;
}
