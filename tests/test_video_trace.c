#include "waydisplay/wd_video_trace.h"

#include <assert.h>

int wd_test_video_trace_c(void) {
    static const uint8_t sample[] = {0, 0, 0, 1, 0x26, 0x01, 0xaa, 0xbb, 0xcc};
    assert(!wd_video_trace_sample(0));
    assert(wd_video_trace_sample(1));
    assert(wd_video_trace_sample(8));
    assert(!wd_video_trace_sample(9));
    assert(wd_video_trace_sample(128));
    assert(wd_video_trace_sample(256));
    assert(!wd_video_trace_sample(129));
    assert(wd_video_trace_prefix(sample, sizeof(sample)) == UINT64_C(0x000000012601aabb));
    assert(wd_video_trace_prefix(sample, 3) == UINT64_C(0x0000000000000000));
    assert(wd_video_trace_prefix(sample, 4) == UINT64_C(0x0000000100000000));
    assert(wd_video_trace_hash(sample, sizeof(sample)) != wd_video_trace_hash(sample, sizeof(sample) - 1));
    assert(wd_video_trace_hash(sample, sizeof(sample)) == wd_video_trace_hash(sample, sizeof(sample)));
    assert(wd_video_trace_hash(NULL, 4) == 0);
    return 1;
}
