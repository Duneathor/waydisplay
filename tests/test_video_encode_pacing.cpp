#include "video_encode_pacing.h"

#include <cassert>
#include <cstdint>

int main() {
    assert(wd_video_encode_pacing_cap(60, 0, 0) == 60);
    assert(wd_video_encode_pacing_cap(60, UINT64_C(77000000), 3) == 60);
    assert(wd_video_encode_pacing_cap(60, UINT64_C(77000000), 4) == 11);
    assert(wd_video_encode_pacing_cap(60, UINT64_C(20000000), 4) == 42);
    assert(wd_video_encode_pacing_cap(60, UINT64_C(10000000), 4) == 60);
    assert(wd_video_encode_pacing_cap(15, UINT64_C(20000000), 4) == 15);
    assert(wd_video_encode_pacing_cap(60, UINT64_C(1000000000), 4) == 5);
    assert(wd_video_encode_pacing_cap(0, UINT64_C(1000000000), 4) == 0);
    assert(wd_video_encode_pacing_ewma(0, UINT64_C(700000000)) == UINT64_C(700000000));
    assert(wd_video_encode_pacing_ewma(UINT64_C(80000000), UINT64_C(40000000)) == UINT64_C(70000000));
    assert(wd_video_encode_pacing_ewma(UINT64_C(40000000), UINT64_C(80000000)) == UINT64_C(50000000));
    assert(wd_video_encode_pacing_ewma(UINT64_MAX - 1, UINT64_MAX) <= UINT64_MAX);
    assert(wd_video_encode_pacing_ewma(1234, 0) == 1234);
    return 0;
}
