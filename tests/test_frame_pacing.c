#include "../src/server/wd_frame_pacing.h"
#include "waydisplay/wd_time.h"

#include <stdio.h>

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr);                                         \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int check_rate(uint16_t fps) {
    struct wd_frame_pacing_state state = {0};
    const uint64_t start = 10u * WD_NSEC_PER_SEC;
    uint64_t frames = 0;

    /* Poll twice per millisecond to model frequent unrelated eventfd wakeups.
     * The absolute deadline must remain authoritative regardless of call rate. */
    for (uint64_t offset = 0; offset < WD_NSEC_PER_SEC; offset += WD_NSEC_PER_MSEC / 2u)
    {
        if (wd_frame_pacing_due(&state, start + offset, fps))
        {
            frames++;
        }
    }

    CHECK(frames <= fps);
    CHECK(frames + 1u >= fps);
    return 0;
}

int main(void) {
    CHECK(wd_frame_service_interval_ms(60, 1, 8) == 8);
    CHECK(wd_frame_service_interval_ms(120, 1, 8) == 8);
    CHECK(wd_frame_service_interval_ms(144, 1, 8) == 6);
    CHECK(wd_frame_service_interval_ms(240, 1, 8) == 4);
    CHECK(wd_frame_service_interval_ms(2000, 1, 8) == 1);

    struct wd_frame_pacing_state state = {0};
    const uint64_t start = 100u * WD_NSEC_PER_SEC;
    CHECK(wd_frame_pacing_due(&state, start, 60));
    CHECK(!wd_frame_pacing_due(&state, start + 16u * WD_NSEC_PER_MSEC, 60));
    CHECK(wd_frame_pacing_due(&state, start + 17u * WD_NSEC_PER_MSEC, 60));
    CHECK(!wd_frame_pacing_due(&state, start + 20u * WD_NSEC_PER_MSEC, 60));

    wd_frame_pacing_reset(&state);
    CHECK(wd_frame_pacing_due(&state, start, 240));
    CHECK(!wd_frame_pacing_due(&state, start + 4u * WD_NSEC_PER_MSEC, 240));
    CHECK(wd_frame_pacing_due(&state, start + 5u * WD_NSEC_PER_MSEC, 240));

    CHECK(check_rate(30) == 0);
    CHECK(check_rate(60) == 0);
    CHECK(check_rate(120) == 0);
    CHECK(check_rate(144) == 0);
    CHECK(check_rate(165) == 0);
    CHECK(check_rate(240) == 0);

    return 0;
}
