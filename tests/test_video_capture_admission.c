#include "video_snapshot_admission.h"
#include "wd_frame_pacing.h"

#include <assert.h>
#include <stdint.h>

static void test_continuous_60hz_compositor(void) {
    struct wd_frame_pacing_state pace = {0};
    unsigned readbacks = 0;
    unsigned snapshots = 0;
    /* Drive the real compositor pacing routine with a 1ms timer over 60s.
     * The real video preflight policy must accept every readback when the
     * encoder and sender are available; there must be no second clock. */
    for (uint64_t now = UINT64_C(1000000); now <= UINT64_C(60000000000); now += UINT64_C(1000000)) {
        if (!wd_frame_pacing_due(&pace, now, 60)) continue;
        readbacks++;
        if (wd_video_snapshot_preflight_decide(true, false) == WD_VIDEO_SNAPSHOT_ACCEPT) snapshots++;
    }
    assert(readbacks >= 3599 && readbacks <= 3601);
    assert(snapshots == readbacks);
    assert(wd_video_snapshot_admission_percent(snapshots, readbacks) == 100.0);
}

static void test_idle_and_backpressure(void) {
    struct wd_frame_pacing_state pace = {0};
    unsigned readbacks = 0;
    unsigned snapshots = 0;
    for (uint64_t now = UINT64_C(1000000); now < UINT64_C(1000000000); now += UINT64_C(50000000)) {
        if (!wd_frame_pacing_due(&pace, now, 60)) continue;
        readbacks++;
        if (wd_video_snapshot_preflight_decide(true, false) == WD_VIDEO_SNAPSHOT_ACCEPT) snapshots++;
    }
    assert(readbacks == 20 && snapshots == 20);
    assert(wd_video_snapshot_preflight_decide(true, true) == WD_VIDEO_SNAPSHOT_PENDING_SEND);
    assert(wd_video_snapshot_preflight_decide(false, false) == WD_VIDEO_SNAPSHOT_UNAVAILABLE);
    assert(wd_video_snapshot_admission_percent(0, 0) == 0.0);
}

int main(void) {
    test_continuous_60hz_compositor();
    test_idle_and_backpressure();
    return 0;
}
