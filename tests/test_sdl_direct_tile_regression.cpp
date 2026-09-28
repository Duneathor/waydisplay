#include "tile_render_policy.hpp"

#include <stdio.h>

using namespace waydisplay;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    /* Regression: the direct tile path has already updated the SDL texture.
     * There are intentionally no legacy dirty rectangles in this case. The
     * renderer must still treat the wake as real remote work so it reaches
     * SDL_RenderPresent instead of leaving the previous (often black) frame. */
    const ClientRemoteTilePresentDecision decision =
        client_remote_tile_present_decide(true, true, false, false, 0);

    CHECK(decision.remote_frame_dirty);
    CHECK(!decision.count_empty_remote_wakeup);

    puts("SDL direct tile present regression: PASS");
    return 0;
}
