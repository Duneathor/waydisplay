#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_zstd.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_tile_count_boundaries() {
    require(wd_tiles_for_width_with_tile(0, 16) == 0, "zero width should have no tiles");
    require(wd_tiles_for_width_with_tile(16, 0) == 0, "zero tile width should be invalid");
    require(wd_tiles_for_width_with_tile(17, 16) == 2, "partial final column should count");
    require(wd_tiles_for_height_with_tile(33, 16) == 3, "partial final row should count");
    require(wd_tiles_for_width_with_tile(UINT32_MAX, 1) == 0, "tile counts beyond the wire ID range should fail closed");
    require(wd_total_tiles_for_size_with_tile(4096, 4096, 1, 1) == 0, "total tile count beyond uint16 should be rejected");
    require(wd_total_tiles_for_size_with_tile(0, 1, 1, 1) == 0, "zero dimension should produce no tile grid");

    require(wd_tiles_for_width(17) == wd_tiles_for_width_with_tile(17, WD_TILE_WIDTH),
            "default width helper should delegate to configured tile width");
    require(wd_tiles_for_height(17) == wd_tiles_for_height_with_tile(17, WD_TILE_HEIGHT),
            "default height helper should delegate to configured tile height");
    require(wd_total_tiles_for_size(17, 17) == wd_total_tiles_for_size_with_tile(17, 17, WD_TILE_WIDTH, WD_TILE_HEIGHT),
            "default total helper should delegate to configured tile size");
}

void test_tile_coordinates_and_visible_edges() {
    require(wd_tile_x_for(7, 0) == 0 && wd_tile_y_for(7, 0) == 0, "zero-column grid should not divide by zero");
    require(wd_tile_x_for(3, 2) == 1 && wd_tile_y_for(3, 2) == 1, "tile ID should map to row-major coordinates");
    require(wd_tile_start_x_for_tile(3, 2, 4) == 4 && wd_tile_start_y_for_tile(3, 2, 2) == 2,
            "tile start should scale row-major coordinates");
    require(wd_tile_visible_width_for_tile(5, 3, 2, 4) == 1, "right edge tile should expose only visible width");
    require(wd_tile_visible_height_for_tile(3, 3, 2, 2) == 1, "bottom edge tile should expose only visible height");
    require(wd_tile_visible_width_for_tile(4, 3, 2, 4) == 0, "fully offscreen tile should have zero visible width");
    require(wd_tile_visible_height_for_tile(2, 3, 2, 2) == 0, "fully offscreen tile should have zero visible height");
    require(wd_tile_id_valid_for(3, 4) && !wd_tile_id_valid_for(4, 4), "tile ID validation should be half-open");

    require(wd_tile_id_valid(0), "default first tile should be valid");
    require(wd_tile_x(1) == 1 && wd_tile_y(WD_TILES_X) == 1, "default coordinate wrappers should remain row-major");
    require(wd_tile_start_x(1) == WD_TILE_WIDTH && wd_tile_start_y(WD_TILES_X) == WD_TILE_HEIGHT,
            "default start wrappers should use configured tile dimensions");
}

void test_pixel_to_protocol_tile_mapping() {
    constexpr uint32_t width = 130, height = 70;

    for (const auto& [tile_width, tile_height] :
         {std::pair<uint16_t, uint16_t>{16, 16}, {32, 32}, {64, 64}, {128, 64}})
    {
        const uint16_t tiles_x = wd_tiles_for_width_with_tile(width, tile_width);
        const uint16_t total_tiles = wd_total_tiles_for_size_with_tile(width, height, tile_width, tile_height);
        uint16_t tile_id = UINT16_MAX;
        require(wd_tile_id_for_pixel(width - 1, height - 1, tiles_x, total_tiles, tile_width, tile_height, &tile_id),
                "bottom-right pixel should map into every supported protocol grid");
        require(tile_id == total_tiles - 1, "bottom-right pixel should map to final protocol tile");

        const uint16_t base_tiles_x = wd_tiles_for_width_with_tile(width, WD_BASE_TILE_WIDTH);
        const uint16_t base_total = wd_total_tiles_for_size_with_tile(width, height, WD_BASE_TILE_WIDTH, WD_BASE_TILE_HEIGHT);
        for (uint16_t base_id = 0; base_id < base_total; ++base_id)
        {
            const uint32_t x = wd_tile_start_x_for_tile(base_id, base_tiles_x, WD_BASE_TILE_WIDTH);
            const uint32_t y = wd_tile_start_y_for_tile(base_id, base_tiles_x, WD_BASE_TILE_HEIGHT);
            uint16_t projected = UINT16_MAX;
            require(wd_tile_id_for_pixel(x, y, tiles_x, total_tiles, tile_width, tile_height, &projected),
                    "every base-grid damage cell should project into the configured protocol grid");
            const uint16_t expected = static_cast<uint16_t>((y / tile_height) * tiles_x + (x / tile_width));
            require(projected == expected, "base-grid damage projection must preserve spatial location");
        }
    }

    uint16_t tile_id = 0;
    require(!wd_tile_id_for_pixel(0, 0, 0, 1, 16, 16, &tile_id), "zero-width grid should be rejected");
    require(!wd_tile_id_for_pixel(UINT32_MAX, UINT32_MAX, 1, 1, 16, 16, &tile_id),
            "coordinates outside the described grid should be rejected");
    require(!wd_tile_id_for_pixel(0, 0, 1, 1, 16, 16, nullptr), "null output should be rejected");
}

void test_extract_hash_and_blit_partial_tiles() {
    constexpr uint32_t width       = 5;
    constexpr uint32_t height      = 3;
    constexpr uint16_t tile_width  = 4;
    constexpr uint16_t tile_height = 2;
    constexpr uint16_t tiles_x     = 2;
    constexpr uint16_t total_tiles = 4;

    std::vector<uint32_t> source(width * height);
    for (size_t i = 0; i < source.size(); ++i)
    {
        source[i] = 0xff000000u | static_cast<uint32_t>(i + 1u);
    }
    std::vector<uint8_t> tile(static_cast<size_t>(tile_width) * tile_height * WD_BYTES_PER_PIXEL, 0xaa);

    require(wd_extract_tile_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 3, tile_width, tile_height, tile.data()),
            "partial corner tile should extract");
    uint32_t extracted_pixel = 0;
    std::memcpy(&extracted_pixel, tile.data(), sizeof(extracted_pixel));
    require(extracted_pixel == source[2 * width + 4], "corner tile should copy its sole visible pixel");
    require(std::all_of(tile.begin() + sizeof(uint32_t), tile.end(), [](uint8_t byte) { return byte == 0; }),
            "padding outside the visible edge should be zeroed");

    std::vector<uint32_t> destination(width * height, 0xdeadbeefu);
    require(
        wd_blit_tile_xrgb8888_for_tile(destination.data(), width, height, tiles_x, total_tiles, 3, tile_width, tile_height, tile.data()),
        "partial corner tile should blit");
    for (size_t i = 0; i < destination.size(); ++i)
    {
        const uint32_t expected = i == 2 * width + 4 ? source[i] : 0xdeadbeefu;
        require(destination[i] == expected, "blit must not overwrite pixels outside the visible tile edge");
    }

    uint32_t hash_before = 0;
    require(wd_fnv1a_tile_hash_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 0, tile_width, tile_height, &hash_before),
            "valid tile hash should succeed");
    source[0] ^= 1u;
    uint32_t hash_after = 0;
    require(wd_fnv1a_tile_hash_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 0, tile_width, tile_height, &hash_after),
            "changed tile hash should succeed");
    require(hash_after != hash_before, "tile hash should reflect visible pixel changes");

    uint32_t invalid_hash = 0x12345678u;
    require(!wd_fnv1a_tile_hash_xrgb8888_for_tile(nullptr, width, height, tiles_x, total_tiles, 0, tile_width, tile_height, &invalid_hash),
            "hash should reject null framebuffer");
    require(invalid_hash == 0x12345678u, "failed hash must not manufacture an output value");
    require(
        !wd_fnv1a_tile_hash_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, total_tiles, tile_width, tile_height, &invalid_hash),
        "hash should reject invalid tile ID");
    require(!wd_extract_tile_xrgb8888_for_tile(nullptr, width, height, tiles_x, total_tiles, 0, tile_width, tile_height, tile.data()),
            "extract should reject null framebuffer");
    require(!wd_extract_tile_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 0, 0, tile_height, tile.data()),
            "extract should reject zero tile width");
    require(!wd_blit_tile_xrgb8888_for_tile(destination.data(), width, height, tiles_x, total_tiles, 0, tile_width, tile_height, nullptr),
            "blit should reject null tile bytes");
}

void test_reject_inconsistent_or_unrepresentable_tile_geometry() {
    const uint32_t framebuffer[] = {0xff102030u};
    uint8_t tile[sizeof(framebuffer)]{};
    uint32_t destination[] = {0xdeadbeefu};

    // 32768 * 32768 * four bytes wraps a uint32_t tile byte count to zero.
    constexpr uint16_t oversized = 32768;
    require(!wd_extract_tile_xrgb8888_for_tile(framebuffer, 1, 1, 1, 1, 0, oversized, oversized, tile),
            "extract must reject a tile size that overflows its byte count");
    require(!wd_blit_tile_xrgb8888_for_tile(destination, 1, 1, 1, 1, 0, oversized, oversized, tile),
            "blit must reject an unrepresentable tile size");
    require(!wd_fnv1a_tile_hash_xrgb8888_for_tile(framebuffer, 1, 1, 1, 1, 0, oversized, oversized, &destination[0]),
            "hash must reject an unrepresentable tile size");
    require(destination[0] == 0xdeadbeefu, "invalid blit must not mutate framebuffer");

    require(!wd_extract_tile_xrgb8888_for_tile(framebuffer, 1, 1, 2, 2, 0, 1, 1, tile),
            "extract must reject a grid that disagrees with framebuffer dimensions");
    require(!wd_blit_tile_xrgb8888_for_tile(destination, 1, 1, 2, 2, 0, 1, 1, tile),
            "blit must reject a grid that disagrees with framebuffer dimensions");
    require(!wd_fnv1a_tile_hash_xrgb8888_for_tile(framebuffer, 1, 1, 2, 2, 0, 1, 1, &destination[0]),
            "hash must reject a grid that disagrees with framebuffer dimensions");
}

void test_sized_tile_buffer_contract() {
    constexpr uint32_t width = 5, height = 3;
    constexpr uint16_t tile_width = 4, tile_height = 2;
    constexpr uint16_t tiles_x = 2, total_tiles = 4;
    uint32_t source[width * height]{};
    uint32_t destination[width * height]{};
    uint8_t tile[tile_width * tile_height * WD_BYTES_PER_PIXEL + 1]{};
    std::memset(tile, 0xa5, sizeof(tile));
    require(!wd_extract_tile_xrgb8888_for_tile_sized(source, width, height, tiles_x, total_tiles, 3,
                                                     tile_width, tile_height, tile, sizeof(tile) - 2),
            "short extraction buffer must be rejected");
    require(std::all_of(std::begin(tile), std::end(tile), [](uint8_t v) { return v == 0xa5; }),
            "short extraction must not touch destination bytes");
    require(wd_extract_tile_xrgb8888_for_tile_sized(source, width, height, tiles_x, total_tiles, 3,
                                                    tile_width, tile_height, tile, sizeof(tile) - 1),
            "exact-sized extraction buffer must succeed");
    require(tile[sizeof(tile) - 1] == 0xa5, "sized extraction must preserve trailing canary");
    require(!wd_blit_tile_xrgb8888_for_tile_sized(destination, width, height, tiles_x, total_tiles, 3,
                                                  tile_width, tile_height, tile, sizeof(tile) - 2),
            "short blit buffer must be rejected");
    require(wd_blit_tile_xrgb8888_for_tile_sized(destination, width, height, tiles_x, total_tiles, 3,
                                                 tile_width, tile_height, tile, sizeof(tile) - 1),
            "exact-sized blit buffer must succeed");
}

void test_zstd_one_shot_and_context_contracts() {
    std::vector<uint8_t> source(8192);
    for (size_t i = 0; i < source.size(); ++i)
    {
        source[i] = static_cast<uint8_t>((i / 32u) & 0xffu);
    }
    const size_t bound = wd_zstd_compress_bound(source.size());
    require(bound >= source.size(), "compression bound should fit the source");
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decoded(source.size());

    uint32_t compressed_size = 99;
    require(wd_zstd_compress(source.data(), source.size(), compressed.data(), compressed.size(), 1, &compressed_size),
            "one-shot compression should succeed");
    require(compressed_size != 0 && compressed_size <= compressed.size(), "compressed size should be bounded");
    require(wd_zstd_decompress(compressed.data(), compressed_size, decoded.data(), decoded.size(), source.size()),
            "one-shot decompression should succeed");
    require(decoded == source, "one-shot compression should round trip exactly");
    require(!wd_zstd_decompress(compressed.data(), compressed_size, decoded.data(), decoded.size(), source.size() - 1),
            "decompression should enforce the expected output size");

    struct wd_zstd_compressor* context = wd_zstd_compressor_create();
    require(context != nullptr, "reusable compression context should be creatable");
    compressed_size = 77;
    require(wd_zstd_compress_with_context(context, source.data(), source.size(), compressed.data(), compressed.size(), 3, &compressed_size),
            "context compression should succeed");
    require(wd_zstd_decompress(compressed.data(), compressed_size, decoded.data(), decoded.size(), source.size()),
            "context-compressed payload should decompress");
    wd_zstd_compressor_destroy(context);
    wd_zstd_compressor_destroy(nullptr);

    compressed_size = 55;
    require(!wd_zstd_compress(nullptr, source.size(), compressed.data(), compressed.size(), 1, &compressed_size) && compressed_size == 0,
            "invalid one-shot input should fail and clear output size");
    compressed_size = 55;
    require(!wd_zstd_compress(source.data(), source.size(), compressed.data(), 1, 1, &compressed_size) && compressed_size == 0,
            "insufficient output capacity should fail cleanly");
    compressed_size = 55;
    require(
        !wd_zstd_compress_with_context(nullptr, source.data(), source.size(), compressed.data(), compressed.size(), 1, &compressed_size) &&
            compressed_size == 0,
        "null context should fail and clear output size");
    require(!wd_zstd_decompress(nullptr, 1, decoded.data(), decoded.size(), source.size()), "decompression should reject null source");
    const uint8_t corrupt[] = {0, 1, 2, 3, 4};
    require(!wd_zstd_decompress(corrupt, sizeof(corrupt), decoded.data(), decoded.size(), source.size()),
            "corrupt compressed data should fail");
    require(wd_zstd_error_name(std::numeric_limits<size_t>::max()) != nullptr,
            "zstd error helper should always return a diagnostic string");
}

} // namespace

int main() {
    test_tile_count_boundaries();
    test_tile_coordinates_and_visible_edges();
    test_pixel_to_protocol_tile_mapping();
    test_extract_hash_and_blit_partial_tiles();
    test_sized_tile_buffer_contract();
    test_reject_inconsistent_or_unrepresentable_tile_geometry();
    test_zstd_one_shot_and_context_contracts();
    return 0;
}
