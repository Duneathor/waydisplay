#include "waydisplay/wd_tile.h"
#include "waydisplay/wd_zstd.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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

    const uint32_t hash_before =
        wd_fnv1a_tile_hash_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 0, tile_width, tile_height);
    source[0] ^= 1u;
    const uint32_t hash_after =
        wd_fnv1a_tile_hash_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 0, tile_width, tile_height);
    require(hash_before != 0 && hash_after != hash_before, "tile hash should reflect visible pixel changes");

    require(wd_fnv1a_tile_hash_xrgb8888_for_tile(nullptr, width, height, tiles_x, total_tiles, 0, tile_width, tile_height) == 0,
            "hash should reject null framebuffer");
    require(
        wd_fnv1a_tile_hash_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, total_tiles, tile_width, tile_height) == 0,
        "hash should reject invalid tile ID");
    require(!wd_extract_tile_xrgb8888_for_tile(nullptr, width, height, tiles_x, total_tiles, 0, tile_width, tile_height, tile.data()),
            "extract should reject null framebuffer");
    require(!wd_extract_tile_xrgb8888_for_tile(source.data(), width, height, tiles_x, total_tiles, 0, 0, tile_height, tile.data()),
            "extract should reject zero tile width");
    require(!wd_blit_tile_xrgb8888_for_tile(destination.data(), width, height, tiles_x, total_tiles, 0, tile_width, tile_height, nullptr),
            "blit should reject null tile bytes");
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
    test_extract_hash_and_blit_partial_tiles();
    test_zstd_one_shot_and_context_contracts();
    return 0;
}
