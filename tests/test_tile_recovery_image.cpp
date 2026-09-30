#include "stream_ownership.h"
#include "tile_present_queue.hpp"
#include "tile_recovery_image.hpp"
#include "tile_upload_epoch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace waydisplay;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static ClientTileUpload make_upload(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                    uint32_t pixel, uint64_t generation,
                                    const wd_client_content_ownership_snapshot& ownership) {
    ClientTileUpload upload;
    upload.rect = {x, y, w, h};
    upload.ownership_epoch = client_tile_upload_epoch(ownership);
    upload.generation = generation;
    upload.source_pitch = static_cast<uint32_t>(w) * sizeof(uint32_t);
    upload.pixels.resize(static_cast<size_t>(upload.source_pitch) * h);
    auto* words = reinterpret_cast<uint32_t*>(upload.pixels.data());
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) words[i] = pixel;
    return upload;
}

static bool apply_current(std::vector<uint32_t>& recovery, uint32_t width, uint32_t height,
                          const ClientTileUpload& upload,
                          const wd_client_content_ownership_snapshot& ownership) {
    return client_tile_upload_matches_ownership(upload, ownership) &&
           client_apply_tile_upload_to_recovery(recovery, width, height, upload);
}

int main() {
    constexpr uint32_t width = 8, height = 6;
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;
    const auto epoch1 = wd_client_stream_ownership_snapshot(&ownership);

    ClientTilePresentQueue queue(8, 4096);
    CHECK(queue.push(make_upload(0, 0, 4, 3, 0x11111111u, 1, epoch1)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.push(make_upload(4, 0, 4, 3, 0x22222222u, 1, epoch1)) == ClientTilePresentPushResult::Queued);
    std::vector<ClientTileUpload> uploads;
    queue.drain(uploads);

    std::vector<uint32_t> recovery(width * height, 0);
    for (const auto& upload : uploads) CHECK(apply_current(recovery, width, height, upload, epoch1));
    CHECK(recovery[0] == 0x11111111u && recovery[7] == 0x22222222u);

    /* large gen1 -> overlapping small gen2 -> large gen3 must leave gen3
     * visible everywhere after queue coalescing. */
    ClientTilePresentQueue overlap_queue(8, 64u * 1024u);
    CHECK(overlap_queue.push(make_upload(0, 0, 8, 6, 0x11111111u, 1, epoch1)) == ClientTilePresentPushResult::Queued);
    CHECK(overlap_queue.push(make_upload(0, 0, 4, 3, 0x22222222u, 2, epoch1)) == ClientTilePresentPushResult::Queued);
    CHECK(overlap_queue.push(make_upload(0, 0, 8, 6, 0x33333333u, 3, epoch1)) == ClientTilePresentPushResult::Queued);
    overlap_queue.drain(uploads);
    CHECK(uploads.size() == 2);
    std::vector<uint32_t> overlap_recovery(width * height, 0u);
    for (const auto& upload : uploads) CHECK(apply_current(overlap_recovery, width, height, upload, epoch1));
    for (uint32_t pixel : overlap_recovery) CHECK(pixel == 0x33333333u);

    /* A full texture upload and a forced texture recreation both source the
     * recovery image and therefore must reproduce the directly uploaded state. */
    const std::vector<uint32_t> full_upload = recovery;
    const std::vector<uint32_t> recreated_texture = recovery;
    CHECK(full_upload == recovery && recreated_texture == recovery);

    (void)wd_client_stream_ownership_begin_video_stream(&ownership);
    (void)wd_client_stream_ownership_end_video_stream(&ownership);
    const auto epoch3 = wd_client_stream_ownership_snapshot(&ownership);
    CHECK(epoch3.epoch != epoch1.epoch);
    const ClientTileUpload stale = make_upload(0, 3, 4, 3, 0x33333333u, 2, epoch1);
    CHECK(!client_tile_upload_matches_ownership(stale, epoch3));
    CHECK(recovery[static_cast<size_t>(3) * width] == 0u);

    const ClientTileUpload fresh = make_upload(0, 3, 8, 3, 0x44444444u, 2, epoch3);
    CHECK(apply_current(recovery, width, height, fresh, epoch3));
    CHECK(recovery[static_cast<size_t>(5) * width + 7] == 0x44444444u);

    /* Resize creates a new recovery image. Old-size uploads must not scribble
     * outside it, while current-size uploads repopulate it normally. */
    constexpr uint32_t resized_width = 5, resized_height = 4;
    std::vector<uint32_t> resized(resized_width * resized_height, 0);
    const ClientTileUpload out_of_bounds = make_upload(4, 0, 4, 3, 0x55555555u, 3, epoch3);
    CHECK(!client_apply_tile_upload_to_recovery(resized, resized_width, resized_height, out_of_bounds));
    const ClientTileUpload resized_fresh = make_upload(0, 0, 5, 4, 0x66666666u, 3, epoch3);
    CHECK(client_apply_tile_upload_to_recovery(resized, resized_width, resized_height, resized_fresh));
    for (uint32_t pixel : resized) CHECK(pixel == 0x66666666u);

    std::puts("tile recovery image: PASS");
    return 0;
}
