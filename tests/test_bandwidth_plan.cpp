#include "wd_bandwidth_plan.h"

#include "waydisplay/wd_config.h"

#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(expr)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(expr))                                                                                                                       \
        {                                                                                                                                  \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                \
            ++failures;                                                                                                                    \
        }                                                                                                                                  \
    } while (0)

uint64_t pct(uint64_t value, uint32_t percent) {
    return (value / 100u) * percent + ((value % 100u) * percent) / 100u;
}
}

int main() {
    constexpr uint64_t link = 12399ull * 1024ull;

    const auto video = wd_bandwidth_plan_build(link, WD_BANDWIDTH_MODE_VIDEO, true, 128000u);
    CHECK(wd_bandwidth_plan_is_valid(&video, WD_BANDWIDTH_MODE_VIDEO));
    CHECK(video.video_bytes_per_second == pct(link, 75u));
    CHECK(video.control_bytes_per_second == pct(link, 10u));
    CHECK(video.audio_cap_bytes_per_second == pct(link, 10u));
    CHECK(video.overhead_bytes_per_second == pct(link, 5u));
    CHECK(video.audio_reserved_bytes_per_second > 128000u / 8u);
    CHECK(video.audio_reserved_bytes_per_second < video.audio_cap_bytes_per_second);
    CHECK(wd_bandwidth_plan_media_bytes(&video, WD_BANDWIDTH_MODE_VIDEO) == video.video_bytes_per_second);

    const auto tiles = wd_bandwidth_plan_build(link, WD_BANDWIDTH_MODE_TILES, true, 128000u);
    CHECK(wd_bandwidth_plan_is_valid(&tiles, WD_BANDWIDTH_MODE_TILES));
    CHECK(tiles.fresh_tile_bytes_per_second == pct(link, 70u));
    CHECK(tiles.repair_bytes_per_second == pct(link, 5u));
    const uint64_t tile_media = wd_bandwidth_plan_media_bytes(&tiles, WD_BANDWIDTH_MODE_TILES);
    const uint64_t nominal_tile_media = pct(link, 75u);
    CHECK(tile_media <= nominal_tile_media);
    CHECK(nominal_tile_media - tile_media <= 1u);

    const auto slow = wd_bandwidth_plan_build(64u * 1024u, WD_BANDWIDTH_MODE_VIDEO, true, 128000u);
    CHECK(wd_bandwidth_plan_is_valid(&slow, WD_BANDWIDTH_MODE_VIDEO));
    CHECK(slow.audio_reserved_bytes_per_second == slow.audio_cap_bytes_per_second);

    const auto silent = wd_bandwidth_plan_build(link, WD_BANDWIDTH_MODE_TILES, false, 0);
    CHECK(silent.audio_reserved_bytes_per_second == 0);

    const uint64_t odd_links[] = {1u, 99u, 101u, 100003u, UINT64_C(123456789)};
    for (uint64_t odd_link : odd_links)
    {
        const auto odd_video = wd_bandwidth_plan_build(odd_link, WD_BANDWIDTH_MODE_VIDEO, true, 128000u);
        const auto odd_tiles = wd_bandwidth_plan_build(odd_link, WD_BANDWIDTH_MODE_TILES, true, 128000u);
        CHECK(wd_bandwidth_plan_is_valid(&odd_video, WD_BANDWIDTH_MODE_VIDEO));
        CHECK(wd_bandwidth_plan_is_valid(&odd_tiles, WD_BANDWIDTH_MODE_TILES));
        CHECK(odd_video.video_bytes_per_second + odd_video.audio_cap_bytes_per_second +
                  odd_video.control_bytes_per_second + odd_video.overhead_bytes_per_second <=
              odd_link);
        CHECK(odd_tiles.fresh_tile_bytes_per_second + odd_tiles.repair_bytes_per_second +
                  odd_tiles.audio_cap_bytes_per_second + odd_tiles.control_bytes_per_second +
                  odd_tiles.overhead_bytes_per_second <=
              odd_link);
    }

    wd_bandwidth_bucket bucket{};
    CHECK(wd_bandwidth_bucket_available(&bucket, 1000u, 250u, 1000000000ull) == 0u);
    CHECK(wd_bandwidth_bucket_available(&bucket, 1000u, 250u, 1100000000ull) == 100u);
    CHECK(wd_bandwidth_bucket_consume(&bucket, 60u) == 60u);
    CHECK(wd_bandwidth_bucket_available(&bucket, 1000u, 250u, 1200000000ull) == 140u);
    CHECK(wd_bandwidth_bucket_consume(&bucket, 500u) == 140u);
    wd_bandwidth_bucket_refund(&bucket, 400u, 250u);
    CHECK(wd_bandwidth_bucket_consume(&bucket, 300u) == 250u);
    wd_bandwidth_bucket_reset(&bucket);
    CHECK(wd_bandwidth_bucket_available(&bucket, 1000u, 250u, 1300000000ull) == 0u);

    return failures == 0 ? 0 : 1;
}
