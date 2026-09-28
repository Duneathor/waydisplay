#include "tile_upload_epoch.hpp"

#include <cstdio>

using namespace waydisplay;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;
    const auto remote_epoch = wd_client_stream_ownership_snapshot(&ownership).epoch;

    /* Applying a server config resets the renderer's local fence even when
     * the transport remains tiles. The remote packet epoch is unchanged. */
    const uint64_t local_epoch = wd_client_stream_ownership_reset_to_tiles(&ownership);
    const auto local = wd_client_stream_ownership_snapshot(&ownership);
    CHECK(local.owner == WD_CLIENT_CONTENT_OWNER_TILES);
    CHECK(local.epoch == local_epoch);
    CHECK(local.epoch != remote_epoch);

    const uint64_t upload_epoch = client_tile_upload_epoch(local);
    CHECK(upload_epoch == local.epoch);

    ClientTileUpload upload;
    upload.ownership_epoch = upload_epoch;
    CHECK(client_tile_upload_matches_ownership(upload, local));

    std::puts("tile upload epoch: PASS");
    return 0;
}
