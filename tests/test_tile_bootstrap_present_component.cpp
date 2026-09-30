#include "tile_present_queue.hpp"
#include "tile_upload_epoch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace waydisplay;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static ClientTileUpload make_upload(uint16_t x, uint8_t value,
                                    const wd_client_content_ownership_snapshot& ownership) {
    ClientTileUpload upload;
    upload.rect = {x, 0, 4, 4};
    upload.ownership_epoch = client_tile_upload_epoch(ownership);
    upload.generation = 1;
    upload.source_pitch = 4u * 4u;
    upload.pixels.assign(4u * 4u * 4u, value);
    return upload;
}

int main() {
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;
    const uint64_t remote_epoch = wd_client_stream_ownership_snapshot(&ownership).epoch;
    (void)wd_client_stream_ownership_reset_to_tiles(&ownership);
    const auto local = wd_client_stream_ownership_snapshot(&ownership);
    CHECK(local.epoch != remote_epoch);

    ClientTilePresentQueue queue(8, 4096);
    CHECK(queue.push(make_upload(0, 0x11, local)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.push(make_upload(4, 0x22, local)) == ClientTilePresentPushResult::Queued);

    std::vector<ClientTileUpload> uploads;
    queue.drain(uploads);
    CHECK(uploads.size() == 2);

    /* This is the non-SDL half of upload_completed_tiles_direct(): only
     * ownership-current uploads are allowed to update the renderer's CPU
     * recovery image. A bootstrap full upload reads this image immediately. */
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 4;
    std::vector<uint32_t> recovery(width * height, 0);
    size_t accepted = 0;
    for (const ClientTileUpload& upload : uploads)
    {
        if (!client_tile_upload_matches_ownership(upload, local))
        {
            continue;
        }
        ++accepted;
        for (uint32_t row = 0; row < upload.rect.h; ++row)
        {
            const uint8_t* src = upload.pixels.data() + static_cast<size_t>(row) * upload.source_pitch;
            uint8_t* dst = reinterpret_cast<uint8_t*>(recovery.data() +
                static_cast<size_t>(upload.rect.y + row) * width + upload.rect.x);
            std::memcpy(dst, src, static_cast<size_t>(upload.rect.w) * 4u);
        }
    }

    CHECK(accepted == 2);
    const auto* bytes = reinterpret_cast<const uint8_t*>(recovery.data());
    CHECK(std::any_of(bytes, bytes + recovery.size() * sizeof(uint32_t), [](uint8_t v) { return v != 0; }));
    CHECK(bytes[0] == 0x11);
    CHECK(bytes[4u * 4u] == 0x22);

    std::puts("tile bootstrap present component: PASS");
    return 0;
}
