#include "video_decoder_conversion.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_coded_padding_is_excluded_from_visible_frame() {
    /* 6x4 coded luma with a 5x3 visible picture. The last coded column and
     * last coded row are sentinels and must never enter the output frame. */
    const std::array<uint8_t, 32> y = {
        1, 2, 3, 4, 5, 0xee, 0xee, 0xee,
        6, 7, 8, 9, 10, 0xee, 0xee, 0xee,
        11, 12, 13, 14, 15, 0xee, 0xee, 0xee,
        0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee,
    };
    /* Coded chroma is 3x2; stride is padded to four bytes. */
    const std::array<uint8_t, 8> u = {31, 32, 33, 0xdd, 34, 35, 36, 0xdd};
    const std::array<uint8_t, 8> v = {41, 42, 43, 0xcc, 44, 45, 46, 0xcc};
    const uint8_t* src[3] = {y.data(), u.data(), v.data()};
    const int stride[3] = {8, 4, 4};

    ClientVideoFrameBuffer output{};
    require(client_video_decoder_copy_visible_planes(ClientVideoPlaneLayout::YUV420P, src, stride,
                                                     6, 4, 5, 3, output),
            "decoder boundary should accept a smaller visible rectangle inside coded storage");
    require(output.valid(), "converted visible frame must satisfy the IYUV buffer contract");
    require(output.width == 5 && output.height == 3 && output.y_pitch == 5 && output.uv_pitch == 3,
            "decoder output geometry must describe the visible picture, not coded padding");

    const std::array<uint8_t, 15> expected_y = {
        1, 2, 3, 4, 5,
        6, 7, 8, 9, 10,
        11, 12, 13, 14, 15,
    };
    for (size_t i = 0; i < expected_y.size(); ++i)
    {
        require(output.bytes[i] == expected_y[i], "coded luma padding leaked into visible output");
    }

    const std::array<uint8_t, 6> expected_u = {31, 32, 33, 34, 35, 36};
    const std::array<uint8_t, 6> expected_v = {41, 42, 43, 44, 45, 46};
    for (size_t i = 0; i < expected_u.size(); ++i)
    {
        require(output.bytes[output.u_offset + i] == expected_u[i],
                "coded U padding leaked into visible output");
        require(output.bytes[output.v_offset + i] == expected_v[i],
                "coded V padding leaked into visible output");
    }
}

void test_nv12_decoder_boundary_preserves_visible_samples() {
    const std::array<uint8_t, 24> y = {
        1, 2, 3, 4, 5, 0xee, 0xee, 0xee,
        6, 7, 8, 9, 10, 0xee, 0xee, 0xee,
        11, 12, 13, 14, 15, 0xee, 0xee, 0xee,
    };
    const std::array<uint8_t, 16> uv = {
        31, 41, 32, 42, 33, 43, 0xdd, 0xdd,
        34, 44, 35, 45, 36, 46, 0xdd, 0xdd,
    };
    const uint8_t* src[3] = {y.data(), uv.data(), nullptr};
    const int stride[3] = {8, 8, 0};

    ClientVideoFrameBuffer output{};
    require(client_video_decoder_copy_visible_planes(ClientVideoPlaneLayout::NV12, src, stride,
                                                     6, 4, 5, 3, output),
            "decoder boundary should directly convert padded NV12");
    require(output.valid(), "NV12 decoder conversion must produce a valid IYUV buffer");
    require(output.bytes[output.u_offset] == 31 && output.bytes[output.u_offset + 5] == 36,
            "NV12 U deinterleave must preserve first and last visible samples");
    require(output.bytes[output.v_offset] == 41 && output.bytes[output.v_offset + 5] == 46,
            "NV12 V deinterleave must preserve first and last visible samples");
}

void test_visible_geometry_cannot_exceed_coded_frame() {
    const std::array<uint8_t, 64> plane{};
    const uint8_t* src[3] = {plane.data(), plane.data(), plane.data()};
    const int stride[3] = {8, 4, 4};
    ClientVideoFrameBuffer output{};

    require(!client_video_decoder_copy_visible_planes(ClientVideoPlaneLayout::YUV420P, src, stride,
                                                      4, 3, 5, 3, output),
            "visible width larger than coded width must be rejected");
    require(!client_video_decoder_copy_visible_planes(ClientVideoPlaneLayout::YUV420P, src, stride,
                                                      5, 2, 5, 3, output),
            "visible height larger than coded height must be rejected");
}

void test_decoder_metadata_comes_from_matched_packet_header() {
    wd_video_frame_payload_header header{};
    header.width = 65;
    header.height = 49;
    header.coded_width = 66;
    header.coded_height = 50;
    header.frame_id = UINT64_C(37);
    header.content_epoch = UINT64_C(9);
    header.pts_usec = UINT64_C(1234567);

    ClientDecodedVideoFrame decoded{};
    client_video_decoder_assign_metadata(header, decoded);
    require(decoded.format == ClientVideoPixelFormat::IYUV,
            "decoder metadata must describe the conversion output format");
    require(decoded.width == 65 && decoded.height == 49,
            "decoder metadata must expose visible dimensions");
    require(decoded.frame_id == UINT64_C(37) && decoded.content_epoch == UINT64_C(9) &&
                decoded.pts_usec == UINT64_C(1234567),
            "frame identity, content epoch, and PTS must survive conversion unchanged");
}

void test_prepare_reuses_exact_visible_layout() {
    ClientVideoFrameBuffer output{};
    require(client_video_decoder_prepare_iyuv(output, 65, 49),
            "odd visible dimensions must allocate an IYUV output");
    require(output.valid(), "prepared odd-sized output must validate");
    const size_t expected_size = static_cast<size_t>(65) * 49 + static_cast<size_t>(33) * 25 * 2;
    require(output.bytes.size() == expected_size,
            "prepared buffer size must round chroma dimensions up exactly");

    output.bytes.assign(output.bytes.size(), 0xab);
    require(client_video_decoder_prepare_iyuv(output, 65, 49),
            "preparing the same geometry should reuse the allocation");
    require(output.bytes.front() == 0xab,
            "same-size preparation should not gratuitously clear decoded storage");
}

} // namespace

int main() {
    test_coded_padding_is_excluded_from_visible_frame();
    test_nv12_decoder_boundary_preserves_visible_samples();
    test_visible_geometry_cannot_exceed_coded_frame();
    test_decoder_metadata_comes_from_matched_packet_header();
    test_prepare_reuses_exact_visible_layout();
    return EXIT_SUCCESS;
}
