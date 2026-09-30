#include "waydisplay/wd_config.h"
#include "wd_bandwidth_plan.h"

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
    require(wd_bandwidth_audio_wire_bytes_per_second(128000) == 19200, "128 kbit/s plus 20% transport allowance should require 19,200 B/s");
    require(wd_bandwidth_audio_select_bitrate(1024u * 1024u, WD_AUDIO_BITRATE_DEFAULT, WD_AUDIO_BITRATE_MIN) == WD_AUDIO_BITRATE_DEFAULT,
            "healthy links should retain the preferred Opus bitrate");
    const uint32_t constrained = wd_bandwidth_audio_select_bitrate(64u * 1024u, WD_AUDIO_BITRATE_DEFAULT, WD_AUDIO_BITRATE_MIN);
    require(constrained >= WD_AUDIO_BITRATE_MIN && constrained < WD_AUDIO_BITRATE_DEFAULT,
            "slow links should negotiate a bitrate that fits the production audio class");
    require(wd_bandwidth_audio_select_bitrate(24u * 1024u, WD_AUDIO_BITRATE_DEFAULT, WD_AUDIO_BITRATE_MIN) == 0,
            "links below the minimum audio quality floor should disable audio");
    return EXIT_SUCCESS;
}
