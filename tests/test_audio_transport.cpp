#include "waydisplay/wd_audio_transport.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    require(wd_audio_reserved_bytes_per_second(128000) == 20000,
            "128 kbit/s should reserve 20,000 bytes per second");
    require(wd_audio_reserve_from_tile_budget(1000000, 128000) == 980000,
            "audio reservation should reduce a large tile budget");
    require(wd_audio_reserve_from_tile_budget(50000, 128000) == 25000,
            "audio reservation should retain the minimum tile budget");
    require(wd_audio_reserve_from_tile_budget(1000000, 0) == 1000000,
            "disabled audio should not change the tile budget");
    return EXIT_SUCCESS;
}
