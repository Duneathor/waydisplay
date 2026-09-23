#include "waydisplay/wd_video_trace.h"

#include <cassert>

extern "C" int wd_test_video_trace_c(void);

int main() {
    static const uint8_t packet[] = {0, 0, 0, 1, 0x26, 0x01, 0xaa, 0xbb, 0xcc};
    uint8_t different[sizeof(packet)] = {};
    for (size_t i = 0; i < sizeof(packet); ++i)
    {
        different[i] = packet[i];
    }
    different[sizeof(packet) - 1] ^= 1;
    assert(wd_video_trace_hash(packet, sizeof(packet)) != wd_video_trace_hash(different, sizeof(different)));
    assert(wd_video_trace_prefix(packet, sizeof(packet)) == wd_video_trace_prefix(different, sizeof(different)));
#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_DEBUG
    assert(wd_video_trace_debug_sample(1));
    assert(wd_video_trace_debug_sample(128));
    assert(!wd_video_trace_debug_sample(127));
#else
    assert(!wd_video_trace_debug_sample(1));
    assert(!wd_video_trace_debug_sample(128));
#endif
    assert(!wd_video_trace_debug_sample(9));
    assert(wd_test_video_trace_c());
    return 0;
}
