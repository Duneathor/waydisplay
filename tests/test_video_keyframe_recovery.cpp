#include "video_keyframe_recovery.h"
#include "waydisplay/wd_protocol.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef WAYDISPLAY_TEST_FIXTURE_DIR
#define WAYDISPLAY_TEST_FIXTURE_DIR "."
#endif

#define CHECK(cond)                                                                                                                         \
    do                                                                                                                                      \
    {                                                                                                                                       \
        if (!(cond))                                                                                                                        \
        {                                                                                                                                   \
            std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);                                                       \
            std::exit(1);                                                                                                                   \
        }                                                                                                                                   \
    } while (false)

namespace {

using Result = wd_client_video_keyframe_result;

std::vector<uint8_t> fixture(const char* name) {
    std::ifstream file(std::string(WAYDISPLAY_TEST_FIXTURE_DIR) + "/" + name, std::ios::binary);
    CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void expect(uint32_t codec, const std::vector<uint8_t>& bytes, Result result) {
    CHECK(wd_client_video_keyframe_validate(codec, bytes.data(), static_cast<uint32_t>(bytes.size())) == result);
}

void test_real_recovery_keyframes() {
    expect(WD_VIDEO_CODEC_H265, fixture("video_keyframe_64x48.h265"), WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_H265, fixture("video_keyframe_128x128.h265"), WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_H264, fixture("video_keyframe_64x48.h264"), WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_H264, fixture("video_keyframe_128x128.h264"), WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_AV1, fixture("video_keyframe_64x48.obu"), WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_AV1, fixture("video_keyframe_128x128.obu"), WD_CLIENT_VIDEO_KEYFRAME_VALID);
}

void test_av1_obu_recovery() {
    /* Minimal synthetic OBU headers and first frame_header byte. Not a decoded fixture. */
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 1, 0, 0x32, 1, 0}, WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_AV1, {0x12, 0, 0x0a, 1, 0, 0x1a, 1, 0, 0x22, 1, 0}, WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_AV1, {0x32, 1, 0}, WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS);
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 1, 0, 0x32, 1, 0x20}, WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 1, 0}, WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 0x80}, WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
           WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 7, 0}, WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_AV1, {0x82, 0}, WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_AV1, {0x0a, 1, 0, 0x32, 0}, WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
}

void test_missing_hevc_headers_and_random_access() {
    constexpr std::array<uint8_t, 6> vps{0, 0, 1, 0x40, 0x01, 0x80};
    constexpr std::array<uint8_t, 6> sps{0, 0, 1, 0x42, 0x01, 0x80};
    constexpr std::array<uint8_t, 6> pps{0, 0, 1, 0x44, 0x01, 0x80};
    constexpr std::array<uint8_t, 7> idr{0, 0, 0, 1, 0x26, 0x01, 0x80};
    constexpr std::array<uint8_t, 6> dependent{0, 0, 1, 0x02, 0x01, 0x80};
    std::vector<uint8_t> unit;
    const auto append = [&unit](const auto& nal) { unit.insert(unit.end(), nal.begin(), nal.end()); };

    append(vps);
    append(sps);
    append(idr);
    expect(WD_VIDEO_CODEC_H265, unit, WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS);
    unit.clear();
    append(vps);
    append(sps);
    append(pps);
    expect(WD_VIDEO_CODEC_H265, unit, WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
    append(dependent);
    expect(WD_VIDEO_CODEC_H265, unit, WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
    unit.clear();
    append(vps);
    append(sps);
    append(pps);
    append(idr);
    expect(WD_VIDEO_CODEC_H265, unit, WD_CLIENT_VIDEO_KEYFRAME_VALID);
}

void test_reject_malformed_and_length_prefixed() {
    expect(WD_VIDEO_CODEC_H265, {0, 0, 0, 5, 0x40, 0x01, 0x00, 0x00, 0x00}, WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_H265, {0, 0, 1, 0x40}, WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_H264, {0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 1, 0, 0, 1, 0x65, 1},
           WD_CLIENT_VIDEO_KEYFRAME_VALID);
    expect(WD_VIDEO_CODEC_H264, {0, 0, 1, 0x67, 1, 0, 0, 1, 0x65, 1}, WD_CLIENT_VIDEO_KEYFRAME_MISSING_PARAMETER_SETS);
    expect(WD_VIDEO_CODEC_H264, {0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 1, 0, 0, 1, 0x41, 1},
           WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
    CHECK(wd_client_video_keyframe_validate(WD_VIDEO_CODEC_H265, nullptr, 0) == WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    CHECK(wd_client_video_keyframe_validate(0, nullptr, 0) == WD_CLIENT_VIDEO_KEYFRAME_INVALID_BITSTREAM);
    expect(WD_VIDEO_CODEC_H265, {0, 0, 1, 0x40, 0x01, 0x80, 0, 0, 1, 0x42, 0x01, 0x80,
                                 0, 0, 1, 0x44, 0x01, 0x80, 0, 0, 1, 0x02, 0x01, 0x80},
           WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
    /* Exercise truncated and arbitrary inputs under the sanitizer build. */
    std::array<uint8_t, 48> noise{};
    uint32_t seed = 0x153947abu;
    for (size_t size = 0; size <= noise.size(); ++size)
    {
        for (auto& byte : noise)
        {
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>(seed >> 24);
        }
        for (uint32_t codec : {WD_VIDEO_CODEC_H264, WD_VIDEO_CODEC_H265, WD_VIDEO_CODEC_AV1})
        {
            const auto result = wd_client_video_keyframe_validate(codec, noise.data(), static_cast<uint32_t>(size));
            CHECK(result >= WD_CLIENT_VIDEO_KEYFRAME_VALID && result <= WD_CLIENT_VIDEO_KEYFRAME_MISSING_RANDOM_ACCESS);
        }
    }
}

} // namespace

int main() {
    test_real_recovery_keyframes();
    test_av1_obu_recovery();
    test_missing_hevc_headers_and_random_access();
    test_reject_malformed_and_length_prefixed();
    return 0;
}
