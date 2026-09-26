#include "video_presentation_geometry.hpp"

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

void require_rect(const ClientVideoPresentationRect& rect, int x, int y, int w, int h,
                  bool pixel_exact, const char* message) {
    require(rect.x == x && rect.y == y && rect.w == w && rect.h == h &&
                rect.pixel_exact == pixel_exact,
            message);
}

void test_exact_physical_pixels_are_the_only_pixel_exact_case() {
    require_rect(client_video_presentation_rect(1422, 773, 1422, 773),
                 0, 0, 1422, 773, true,
                 "matching physical output and source must render pixel for pixel");
    require_rect(client_video_presentation_rect(2844, 1546, 1422, 773),
                 0, 0, 2844, 1546, false,
                 "2x high-DPI scaling is not a physical 1:1 presentation");
}

void test_fractional_high_dpi_scale_uses_integer_destination_pixels() {
    require_rect(client_video_presentation_rect(2133, 1160, 1422, 773),
                 0, 0, 2133, 1159, false,
                 "1.5x high-DPI output should preserve aspect ratio with integer destination pixels");
    require_rect(client_video_presentation_rect(1778, 965, 1422, 773),
                 1, 0, 1775, 965, false,
                 "fractional scaling should constrain the dimension that would otherwise exceed the output");
}

void test_letterbox_and_pillarbox_are_centered() {
    require_rect(client_video_presentation_rect(1920, 1080, 800, 600),
                 240, 0, 1440, 1080, false,
                 "4:3 content on 16:9 output must be centered with side bars");
    require_rect(client_video_presentation_rect(800, 1200, 1920, 1080),
                 0, 375, 800, 450, false,
                 "16:9 content on portrait output must be centered with top and bottom bars");
}

void test_odd_destination_remainders_are_deterministic() {
    require_rect(client_video_presentation_rect(101, 101, 16, 9),
                 0, 22, 101, 56, false,
                 "odd leftover rows should use deterministic integer centering");
    require_rect(client_video_presentation_rect(101, 101, 9, 16),
                 22, 0, 56, 101, false,
                 "odd leftover columns should use deterministic integer centering");
}

void test_invalid_output_and_source_dimensions_stay_nonempty() {
    require_rect(client_video_presentation_rect(0, 0, 0, 0),
                 0, 0, 1, 1, false,
                 "zero output and source dimensions must not produce an empty destination");
    require_rect(client_video_presentation_rect(-40, -20, 640, 480),
                 0, 0, 1, 1, false,
                 "negative output dimensions must clamp to one pixel");
    require_rect(client_video_presentation_rect(1280, 720, 0, 720),
                 0, 0, 1280, 720, false,
                 "missing source width should use the whole valid output without claiming 1:1 mapping");
}

void test_tiny_output_never_rounds_a_dimension_to_zero() {
    const ClientVideoPresentationRect wide = client_video_presentation_rect(1, 9, 3840, 2160);
    require(wide.w == 1 && wide.h == 1 && wide.x == 0 && wide.y == 4,
            "wide source on a one-pixel output must keep a nonzero destination");
    const ClientVideoPresentationRect tall = client_video_presentation_rect(9, 1, 1080, 1920);
    require(tall.w == 1 && tall.h == 1 && tall.x == 4 && tall.y == 0,
            "tall source on a one-pixel output must keep a nonzero destination");
}

} // namespace

int main() {
    test_exact_physical_pixels_are_the_only_pixel_exact_case();
    test_fractional_high_dpi_scale_uses_integer_destination_pixels();
    test_letterbox_and_pillarbox_are_centered();
    test_odd_destination_remainders_are_deterministic();
    test_invalid_output_and_source_dimensions_stay_nonempty();
    test_tiny_output_never_rounds_a_dimension_to_zero();
    return EXIT_SUCCESS;
}
