#include "waydisplay/wd_audio_transport.h"
#include <cassert>
int main() {
    assert(wd_audio_reserved_bytes_per_second(128000) == 20000);
    assert(wd_audio_reserve_from_tile_budget(1000000, 128000) == 980000);
    assert(wd_audio_reserve_from_tile_budget(50000, 128000) == 25000);
    assert(wd_audio_reserve_from_tile_budget(1000000, 0) == 1000000);
    return 0;
}
