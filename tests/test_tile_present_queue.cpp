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
    CHECK(queue.push(make_upload(0, 0, 16, 16, 1, 1, 0x11)));
    CHECK(queue.size() == 1);
    CHECK(queue.bytes() == 1024);

    /* Same rectangle/epoch coalesces in place and newest generation wins. */
    CHECK(queue.push(make_upload(0, 0, 16, 16, 1, 2, 0x22)));
    CHECK(queue.size() == 1);
    CHECK(queue.push(make_upload(0, 0, 16, 16, 1, 1, 0x33)));
    CHECK(queue.size() == 1);

    CHECK(queue.push(make_upload(16, 0, 16, 16, 1, 1, 0x44)));
    CHECK(queue.size() == 2);
    CHECK(!queue.push(make_upload(32, 0, 16, 16, 1, 1, 0x55)));

    std::vector<ClientTileUpload> drained;
    queue.drain(drained);
    CHECK(drained.size() == 2);
    CHECK(queue.empty());
    CHECK(queue.bytes() == 0);
    CHECK(drained[0].generation == 2);
    CHECK(drained[0].pixels[0] == 0x22);

    /* Epoch changes must not coalesce because ownership can change between
     * network completion and renderer consumption. */
    CHECK(queue.push(make_upload(0, 0, 8, 8, 7, 1, 1)));
    CHECK(queue.push(make_upload(0, 0, 8, 8, 8, 1, 2)));
    CHECK(queue.size() == 2);
    queue.clear();
    CHECK(queue.empty());

    ClientTilePresentQueue tiny(8, 16);
    CHECK(!tiny.push(make_upload(0, 0, 4, 4, 1, 1, 1)));

    puts("tile present queue: PASS");
    return 0;
}
