#include "wd_video_transition.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(expr)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                         \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

int main() {
    CHECK(wd_planned_video_resume_decide(false, false, false, WD_VIDEO_MODE_FORCE, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_WAIT);
    CHECK(wd_planned_video_resume_decide(true, true, false, WD_VIDEO_MODE_FORCE, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_WAIT);
    CHECK(wd_planned_video_resume_decide(true, false, true, WD_VIDEO_MODE_AUTO, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_WAIT);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_FORCE, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_ENTER);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_AUTO, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_ENTER);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_OFF, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_CLEAR);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_FORCE, false, true, true) ==
          WD_PLANNED_VIDEO_RESUME_CLEAR);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_FORCE, true, false, true) ==
          WD_PLANNED_VIDEO_RESUME_CLEAR);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_FORCE, true, true, false) ==
          WD_PLANNED_VIDEO_RESUME_CLEAR);

    CHECK(wd_tile_recovery_generation_decide(false, false, 4, 4, true) ==
          WD_TILE_RECOVERY_GENERATION_WAIT);
    CHECK(wd_tile_recovery_generation_decide(true, true, 4, 4, true) ==
          WD_TILE_RECOVERY_GENERATION_WAIT);
    CHECK(wd_tile_recovery_generation_decide(true, false, 4, 5, true) ==
          WD_TILE_RECOVERY_GENERATION_STALE);
    CHECK(wd_tile_recovery_generation_decide(true, false, 5, 5, false) ==
          WD_TILE_RECOVERY_GENERATION_WAIT);
    CHECK(wd_tile_recovery_generation_decide(true, false, 5, 5, true) ==
          WD_TILE_RECOVERY_GENERATION_TRANSMITTED);
    /* Rapid resize coalescing always invalidates an older recovery barrier and
     * accepts only the newest framebuffer generation. */
    for (uint64_t generation = 10; generation < 20; ++generation)
    {
        CHECK(wd_tile_recovery_generation_decide(true, false, generation, generation + 1, true) ==
              WD_TILE_RECOVERY_GENERATION_STALE);
    }
    CHECK(wd_tile_recovery_generation_decide(true, false, 20, 20, true) ==
          WD_TILE_RECOVERY_GENERATION_TRANSMITTED);
    return 0;
}
