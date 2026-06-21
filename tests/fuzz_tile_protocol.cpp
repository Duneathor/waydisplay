#include "waydisplay/wd_protocol.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    wd_udp_tile_packet_decoded decoded{};
    (void)wd_udp_tile_packet_decode(data, size, &decoded);
    return 0;
}
