#include "tile_present_queue.hpp"

#include <stdio.h>
#include <vector>

using namespace waydisplay;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static ClientTileUpload make_upload(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                    uint64_t epoch, uint64_t generation, uint8_t value) {
    ClientTileUpload upload;
    upload.rect = {x, y, w, h};
    upload.ownership_epoch = epoch;
    upload.generation = generation;
    upload.source_pitch = static_cast<uint32_t>(w) * 4u;
    upload.pixels.assign(static_cast<size_t>(w) * h * 4u, value);
    return upload;
}

int main() {
    ClientTilePresentQueue queue(2, 4096);
    CHECK(queue.push(make_upload(0, 0, 16, 16, 1, 1, 0x11)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.size() == 1);
    CHECK(queue.bytes() == 1024);

    /* Same rectangle/epoch coalesces in place and newest generation wins. */
    CHECK(queue.push(make_upload(0, 0, 16, 16, 1, 2, 0x22)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.size() == 1);
    CHECK(queue.push(make_upload(0, 0, 16, 16, 1, 1, 0x33)) == ClientTilePresentPushResult::Superseded);
    CHECK(queue.size() == 1);

    CHECK(queue.push(make_upload(16, 0, 16, 16, 1, 1, 0x44)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.size() == 2);
    CHECK(queue.push(make_upload(32, 0, 16, 16, 1, 1, 0x55)) == ClientTilePresentPushResult::Rejected);

    std::vector<ClientTileUpload> drained;
    queue.drain(drained);
    CHECK(drained.size() == 2);
    CHECK(queue.empty());
    CHECK(queue.bytes() == 0);
    CHECK(drained[0].generation == 2);
    CHECK(drained[0].pixels[0] == 0x22);

    /* Exact-rectangle supersession must preserve chronological order relative
     * to differently-sized overlapping uploads. */
    ClientTilePresentQueue overlap(8, 64u * 1024u);
    CHECK(overlap.push(make_upload(0, 0, 32, 32, 3, 1, 0x11)) == ClientTilePresentPushResult::Queued);
    CHECK(overlap.push(make_upload(0, 0, 16, 16, 3, 2, 0x22)) == ClientTilePresentPushResult::Queued);
    CHECK(overlap.push(make_upload(0, 0, 32, 32, 3, 3, 0x33)) == ClientTilePresentPushResult::Queued);
    overlap.drain(drained);
    CHECK(drained.size() == 2);
    CHECK(drained[0].rect.w == 16 && drained[0].generation == 2);
    CHECK(drained[1].rect.w == 32 && drained[1].generation == 3);

    /* Epoch changes must not coalesce because ownership can change between
     * network completion and renderer consumption. */
    CHECK(queue.push(make_upload(0, 0, 8, 8, 7, 1, 1)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.push(make_upload(0, 0, 8, 8, 8, 1, 2)) == ClientTilePresentPushResult::Queued);
    CHECK(queue.size() == 2);
    queue.clear();
    CHECK(queue.empty());

    ClientTilePresentQueue tiny(8, 16);
    CHECK(tiny.push(make_upload(0, 0, 4, 4, 1, 1, 1)) == ClientTilePresentPushResult::Rejected);

    puts("tile present queue: PASS");
    return 0;
}
