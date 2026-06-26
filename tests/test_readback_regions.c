#include "wd_readback_regions.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr);                                         \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void) {
    struct wd_readback_region regions[8];
    bool                      full = false;
    bool                      damage[16];

    memset(damage, 0, sizeof(damage));
    damage[0]  = true;
    damage[15] = true;
    size_t count = wd_readback_plan_regions(false, damage, 2, 16, 4, 64, 64, 256, 256, regions, 8, &full);
    CHECK(count == 2);
    CHECK(!full);
    CHECK(regions[0].x == 0 && regions[0].y == 0 && regions[0].width == 64 && regions[0].height == 64);
    CHECK(regions[1].x == 192 && regions[1].y == 192 && regions[1].width == 64 && regions[1].height == 64);

    memset(damage, 0, sizeof(damage));
    damage[1] = true;
    damage[5] = true;
    count     = wd_readback_plan_regions(false, damage, 2, 16, 4, 64, 64, 256, 256, regions, 8, &full);
    CHECK(count == 1);
    CHECK(regions[0].x == 64 && regions[0].y == 0 && regions[0].width == 64 && regions[0].height == 128);

    memset(damage, 0, sizeof(damage));
    damage[0]  = true;
    damage[2]  = true;
    damage[8]  = true;
    damage[10] = true;
    count      = wd_readback_plan_regions(false, damage, 4, 16, 4, 64, 64, 256, 256, regions, 2, &full);
    CHECK(count == 1);
    CHECK(regions[0].x == 0 && regions[0].y == 0 && regions[0].width == 192 && regions[0].height == 192);

    count = wd_readback_plan_regions(true, damage, 4, 16, 4, 64, 64, 256, 256, regions, 8, &full);
    CHECK(count == 1 && full);
    CHECK(regions[0].width == 256 && regions[0].height == 256);

    return 0;
}
