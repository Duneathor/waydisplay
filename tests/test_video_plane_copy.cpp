#include "video_plane_copy.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <size_t N>
void require_bytes(const std::array<uint8_t, N>& actual, const std::array<uint8_t, N>& expected, const char* message) {
    require(actual == expected, message);
}

void test_yuv420p_odd_visible_region_ignores_padding() {
    const std::array<uint8_t, 18> y = {1, 2, 3, 0xee, 0xee, 0xee,
                                       4, 5, 6, 0xee, 0xee, 0xee,
                                       7, 8, 9, 0xee, 0xee, 0xee};
    const std::array<uint8_t, 6> u = {11, 12, 0xdd, 13, 14, 0xdd};
    const std::array<uint8_t, 6> v = {21, 22, 0xcc, 23, 24, 0xcc};
    const uint8_t* src[3] = {y.data(), u.data(), v.data()};
    const int stride[3] = {6, 3, 3};

    std::array<uint8_t, 9> out_y{};
    std::array<uint8_t, 4> out_u{};
    std::array<uint8_t, 4> out_v{};
    uint8_t* dst[3] = {out_y.data(), out_u.data(), out_v.data()};

    require(client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, src, stride, dst, 3, 3),
            "padded planar 4:2:0 input should use the direct visible-plane copy");
    require_bytes(out_y, std::array<uint8_t, 9>{1, 2, 3, 4, 5, 6, 7, 8, 9},
                  "luma padding must never leak into the visible picture");
    require_bytes(out_u, std::array<uint8_t, 4>{11, 12, 13, 14},
                  "U padding must never leak into odd-sized visible chroma");
    require_bytes(out_v, std::array<uint8_t, 4>{21, 22, 23, 24},
                  "V padding must never leak into odd-sized visible chroma");
}

void test_yuv420p_even_dimensions_copy_exact_rows() {
    const std::array<uint8_t, 12> y = {1, 2, 3, 4, 0xee, 0xee,
                                       5, 6, 7, 8, 0xee, 0xee};
    const std::array<uint8_t, 4> u = {11, 12, 0xdd, 0xdd};
    const std::array<uint8_t, 4> v = {21, 22, 0xcc, 0xcc};
    const uint8_t* src[3] = {y.data(), u.data(), v.data()};
    const int stride[3] = {6, 4, 4};

    std::array<uint8_t, 8> out_y{};
    std::array<uint8_t, 2> out_u{};
    std::array<uint8_t, 2> out_v{};
    uint8_t* dst[3] = {out_y.data(), out_u.data(), out_v.data()};

    require(client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, src, stride, dst, 4, 2),
            "even planar dimensions should copy without conversion");
    require_bytes(out_y, std::array<uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8},
                  "even luma rows must be packed without source padding");
    require_bytes(out_u, std::array<uint8_t, 2>{11, 12}, "even U row must copy exactly");
    require_bytes(out_v, std::array<uint8_t, 2>{21, 22}, "even V row must copy exactly");
}

void test_nv12_odd_dimensions_deinterleave_without_padding() {
    constexpr uint32_t width = 5;
    constexpr uint32_t height = 3;
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

    std::array<uint8_t, width * height> out_y{};
    std::array<uint8_t, 6> out_u{};
    std::array<uint8_t, 6> out_v{};
    uint8_t* dst[3] = {out_y.data(), out_u.data(), out_v.data()};

    require(client_copy_video_planes(ClientVideoPlaneLayout::NV12, src, stride, dst, width, height),
            "padded NV12 should deinterleave the visible region directly");
    require_bytes(out_y, std::array<uint8_t, 15>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                  "NV12 luma padding must be excluded");
    require_bytes(out_u, std::array<uint8_t, 6>{31, 32, 33, 34, 35, 36},
                  "NV12 U samples must deinterleave without padded bytes");
    require_bytes(out_v, std::array<uint8_t, 6>{41, 42, 43, 44, 45, 46},
                  "NV12 V samples must deinterleave without padded bytes");
}

void test_invalid_strides_force_fallback() {
    const std::array<uint8_t, 64> plane{};
    const uint8_t* yuv_src[3] = {plane.data(), plane.data(), plane.data()};
    std::array<uint8_t, 64> out{};
    uint8_t* dst[3] = {out.data(), out.data(), out.data()};

    const int bad_y_stride[3] = {4, 3, 3};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, yuv_src, bad_y_stride, dst, 5, 3),
            "short luma stride must reject the direct-copy path");

    const int bad_u_stride[3] = {5, 2, 3};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, yuv_src, bad_u_stride, dst, 5, 3),
            "short planar U stride must reject the direct-copy path");

    const int bad_v_stride[3] = {5, 3, 2};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, yuv_src, bad_v_stride, dst, 5, 3),
            "short planar V stride must reject the direct-copy path");

    const uint8_t* nv12_src[3] = {plane.data(), plane.data(), nullptr};
    const int bad_nv12_stride[3] = {5, 5, 0};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::NV12, nv12_src, bad_nv12_stride, dst, 5, 3),
            "NV12 chroma stride must hold two bytes for every visible chroma sample");
}

void test_invalid_planes_and_dimensions_reject_direct_copy() {
    const std::array<uint8_t, 16> plane{};
    const uint8_t* src[3] = {plane.data(), plane.data(), plane.data()};
    const int stride[3] = {4, 2, 2};
    std::array<uint8_t, 16> out{};
    uint8_t* dst[3] = {out.data(), out.data(), out.data()};

    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, src, stride, dst, 0, 4),
            "zero width must reject the direct-copy path");
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, src, stride, dst, 4, 0),
            "zero height must reject the direct-copy path");

    const uint8_t* missing_y[3] = {nullptr, plane.data(), plane.data()};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, missing_y, stride, dst, 4, 4),
            "missing luma storage must reject the direct-copy path");

    const uint8_t* missing_v[3] = {plane.data(), plane.data(), nullptr};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, missing_v, stride, dst, 4, 4),
            "planar input requires a V plane");

    uint8_t* missing_dst[3] = {out.data(), nullptr, out.data()};
    require(!client_copy_video_planes(ClientVideoPlaneLayout::YUV420P, src, stride, missing_dst, 4, 4),
            "all output planes are required");
}

} // namespace

int main() {
    test_yuv420p_odd_visible_region_ignores_padding();
    test_yuv420p_even_dimensions_copy_exact_rows();
    test_nv12_odd_dimensions_deinterleave_without_padding();
    test_invalid_strides_force_fallback();
    test_invalid_planes_and_dimensions_reject_direct_copy();
    return EXIT_SUCCESS;
}
