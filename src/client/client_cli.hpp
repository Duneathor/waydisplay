#pragma once

#include <cstdint>
#include <string>

namespace waydisplay {

enum class ClientCliParseResult {
    Ok,
    Help,
    Error,
};

struct ClientCliOptions {
    std::string server_host;
    uint16_t tcp_port = 0;
    uint16_t client_udp_port = 0;
    uint16_t desired_width = 0;
    uint16_t desired_height = 0;
    uint16_t target_fps = 0;
    uint32_t limited_udp_kib_per_second = 0;
    uint8_t video_mode = 0;
    uint32_t video_codec_mask = 0;
    uint8_t video_hwdecode_mode = 0;
    bool disable_vsync = false;
    bool disable_audio = false;
};

ClientCliParseResult client_cli_parse(int argc, const char* const* argv,
                                      ClientCliOptions& options,
                                      std::string* error_message = nullptr);

} // namespace waydisplay
