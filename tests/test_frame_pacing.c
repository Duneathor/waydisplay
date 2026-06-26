#include "../src/server/wd_frame_pacing.h"

#include "waydisplay/wd_time.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(expr)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                            \
            return 1;                                                                                                                      \
        }                                                                                                                                  \
    } while (0)

int main(void) {
    CHECK(wd_frame_service_interval_ms(60, 1, 8) == 8);
    CHECK(wd_frame_service_interval_ms(120, 1, 8) == 8);
    CHECK(wd_frame_service_interval_ms(144, 1, 8) == 6);
    CHECK(wd_frame_service_interval_ms(240, 1, 8) == 4);
    CHECK(wd_frame_service_interval_ms(2000, 1, 8) == 1);

    const uint64_t start = 10 * WD_NSEC_PER_SEC;
    CHECK(!wd_frame_pacing_due(start, start + 7 * WD_NSEC_PER_MSEC, 60, 8));
    CHECK(wd_frame_pacing_due(start, start + 9 * WD_NSEC_PER_MSEC, 60, 8));
    CHECK(!wd_frame_pacing_due(start, start + 3 * WD_NSEC_PER_MSEC, 240, 1));
    CHECK(wd_frame_pacing_due(start, start + 4 * WD_NSEC_PER_MSEC, 240, 1));
    CHECK(wd_frame_pacing_due(0, start, 60, 8));
    return 0;
}
