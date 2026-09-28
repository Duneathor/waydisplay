#include "tile_render_policy.hpp"

#include <stdio.h>

using namespace waydisplay;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    ClientRemoteTilePresentDecision decision =
        client_remote_tile_present_decide(false, false, false, false, 0);
    CHECK(!decision.remote_frame_dirty);
    CHECK(!decision.count_empty_remote_wakeup);

    decision = client_remote_tile_present_decide(true, false, false, false, 2);
    CHECK(decision.remote_frame_dirty);
    CHECK(!decision.count_empty_remote_wakeup);

    decision = client_remote_tile_present_decide(true, false, true, false, 0);
    CHECK(decision.remote_frame_dirty);
    CHECK(!decision.count_empty_remote_wakeup);

    decision = client_remote_tile_present_decide(true, false, false, true, 0);
    CHECK(decision.remote_frame_dirty);
    CHECK(!decision.count_empty_remote_wakeup);

    decision = client_remote_tile_present_decide(true, false, false, false, 0);
    CHECK(!decision.remote_frame_dirty);
    CHECK(decision.count_empty_remote_wakeup);

    puts("remote tile present policy: PASS");
    return 0;
}
