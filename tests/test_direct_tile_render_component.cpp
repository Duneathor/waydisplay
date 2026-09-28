#include "tile_present_queue.hpp"
#include "tile_render_policy.hpp"

#include <stdio.h>
#include <vector>

using namespace waydisplay;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static ClientTileUpload make_upload() {
    ClientTileUpload upload;
    upload.rect          = {0, 0, 16, 16};
    upload.ownership_epoch = 7;
    upload.generation    = 3;
    upload.source_pitch  = 16u * 4u;
    upload.pixels.assign(16u * 16u * 4u, 0x5a);
    return upload;
}

int main() {
    ClientTilePresentQueue queue(8, 64u * 1024u);
    CHECK(queue.push(make_upload()));

    /* Model the renderer side of the network->direct-upload handoff: draining
     * a nonempty queue means SDL_UpdateTexture has presentable remote pixels,
     * while the legacy dirty-rectangle count remains zero by design. */
    std::vector<ClientTileUpload> uploads;
    queue.drain(uploads);
    CHECK(uploads.size() == 1);
    CHECK(uploads[0].valid());

    const bool direct_tile_updated = !uploads.empty();
    const ClientRemoteTilePresentDecision decision =
        client_remote_tile_present_decide(true, direct_tile_updated, false, false, 0);
    const bool tile_texture_updated   = direct_tile_updated;
    const bool remote_texture_updated = tile_texture_updated && decision.remote_frame_dirty;
    const bool should_present         = remote_texture_updated;

    CHECK(decision.remote_frame_dirty);
    CHECK(!decision.count_empty_remote_wakeup);
    CHECK(should_present);

    /* A genuine wake with neither direct nor legacy tile work is still empty. */
    const ClientRemoteTilePresentDecision empty =
        client_remote_tile_present_decide(true, false, false, false, 0);
    CHECK(!empty.remote_frame_dirty);
    CHECK(empty.count_empty_remote_wakeup);

    puts("direct tile render component: PASS");
    return 0;
}
