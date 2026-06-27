#include "render_planning.hpp"
#include "wd_video_transition.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(expr)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                        \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

int main() {
    using namespace waydisplay;

    /* A planned resize keeps the currently rendered surface while the server
     * restarts its recovery barrier against the newest framebuffer generation. */
    CHECK(client_render_surface_handoff_decide(false, true, true) == ClientRenderSurfaceHandoff::KeepCurrent);
    CHECK(wd_planned_video_resume_decide(true, true, false, WD_VIDEO_MODE_FORCE, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_WAIT);
    CHECK(wd_tile_recovery_generation_decide(true, false, 7, 8, true) == WD_TILE_RECOVERY_GENERATION_STALE);

    /* The replacement tile surface remains pending until every base tile for
     * the newest recovery frame has arrived and presentation succeeds. */
    std::vector<uint64_t> recovery_tiles{8, 8, 0, 8};
    CHECK(!client_tile_frame_complete(recovery_tiles));
    CHECK(client_render_surface_handoff_decide(true, true, false) == ClientRenderSurfaceHandoff::KeepCurrent);
    recovery_tiles[2] = 8;
    CHECK(client_tile_frame_complete(recovery_tiles));
    CHECK(wd_tile_recovery_generation_decide(true, false, 8, 8, true) == WD_TILE_RECOVERY_GENERATION_TRANSMITTED);
    CHECK(client_render_surface_handoff_decide(true, true, true) == ClientRenderSurfaceHandoff::CommitNew);

    /* Exact recovery presentation resumes both forced and previously selected
     * automatic video without re-running content qualification. */
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_FORCE, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_ENTER);
    CHECK(wd_planned_video_resume_decide(true, false, false, WD_VIDEO_MODE_AUTO, true, true, true) ==
          WD_PLANNED_VIDEO_RESUME_ENTER);

    /* A 34 ms decoder settles at the measured safe rate instead of repeatedly
     * jumping from the ceiling to a deep multiplicative reduction. */
    uint16_t fps = wd_video_cadence_downshift_target(30, 30, 25, false, 5, 2, 75);
    CHECK(fps == 25);
    for (unsigned i = 0; i < 60; ++i)
    {
        fps = wd_video_cadence_upshift_target(fps, 30, 25, 2, 1);
        CHECK(fps == 25);
        fps = wd_video_cadence_downshift_target(fps, 30, 25, false, 5, 2, 75);
        CHECK(fps == 25);
    }

    return 0;
}
