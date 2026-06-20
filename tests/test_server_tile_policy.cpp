#include "wd_tile_policy.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_wire_bytes_account_for_one_extended_header() {
    require(wd_tile_wire_bytes_for_payload(1000, 400, 20, 28) == 1068,
            "three packets should include one extended and two base headers");
    require(wd_tile_wire_bytes_for_payload(400, 400, 20, 28) == 428,
            "single packet should use the first-packet header size");
}

void test_compression_requires_material_savings() {
    require(!wd_tile_compression_is_worthwhile(980, 1000, 400, 20, 20, 64, 3),
            "minor byte savings should not pay compression cost");
    require(wd_tile_compression_is_worthwhile(700, 1000, 400, 20, 20, 64, 3),
            "material wire savings should select compression");
    require(!wd_tile_compression_is_worthwhile(1000, 1000, 400, 20, 20, 0, 0),
            "ties should prefer uncompressed payloads");
}

void test_locality_with_starvation_bound() {
    const std::array<uint16_t, 4> ids{0, 7, 9, 15};
    std::array<uint64_t, 16> queued{};
    queued[0] = 900;
    queued[7] = 800;
    queued[9] = 700;
    queued[15] = 100;
    require(wd_tile_select_local_region_index(ids.data(), ids.size(), 4, 8,
                                              queued.data(), queued.size(), 1000, 0) == 2,
            "nearest region should win without starvation");
    require(wd_tile_select_local_region_index(ids.data(), ids.size(), 4, 8,
                                              queued.data(), queued.size(), 1000, 500) == 3,
            "oldest region should win once starvation threshold is reached");
}

void test_xrgb_compression_prefilter() {
    std::vector<uint32_t> flat(256, 0x00112233u);
    require(wd_tile_xrgb_payload_may_compress(reinterpret_cast<const uint8_t*>(flat.data()),
                                              static_cast<uint32_t>(flat.size() * sizeof(uint32_t))),
            "flat tiles should reach the compressor");

    std::vector<uint32_t> gradient(256);
    for (uint32_t i = 0; i < gradient.size(); ++i)
    {
        gradient[i] = i;
    }
    require(wd_tile_xrgb_payload_may_compress(reinterpret_cast<const uint8_t*>(gradient.data()),
                                              static_cast<uint32_t>(gradient.size() * sizeof(uint32_t))),
            "regular gradients should reach the compressor");

    std::vector<uint32_t> noise(256);
    uint32_t state = 0x12345678u;
    for (uint32_t& pixel : noise)
    {
        state = state * 1664525u + 1013904223u;
        pixel = state & 0x00ffffffu;
    }
    require(!wd_tile_xrgb_payload_may_compress(reinterpret_cast<const uint8_t*>(noise.data()),
                                               static_cast<uint32_t>(noise.size() * sizeof(uint32_t))),
            "high-entropy tiles should bypass zstd");
}

void test_delivery_status_waits_for_seal_and_reports_failure() {
    wd_tile_delivery_status status{};
    wd_tile_delivery_status_add(&status);
    wd_tile_delivery_status_add(&status);
    bool failed = false;
    require(!wd_tile_delivery_status_complete(&status, true, &failed),
            "completion before seal should not finalize a tile");
    require(!wd_tile_delivery_status_seal(&status, &failed),
            "sealed tile should wait for remaining packets");
    require(wd_tile_delivery_status_complete(&status, false, &failed),
            "last packet should finalize a sealed tile");
    require(failed, "any failed packet should fail the tile delivery");
}

} // namespace

int main() {
    test_wire_bytes_account_for_one_extended_header();
    test_compression_requires_material_savings();
    test_locality_with_starvation_bound();
    test_xrgb_compression_prefilter();
    test_delivery_status_waits_for_seal_and_reports_failure();
    return 0;
}
