#pragma once

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum wd_server_cli_parse_result {
    WD_SERVER_CLI_ERROR = -1,
    WD_SERVER_CLI_OK    = 0,
    WD_SERVER_CLI_HELP  = 1,
};

struct wd_server_cli_options {
    const char*    app_command;
    struct in_addr listen_address;
    uint16_t       tcp_port;
    double         output_scale;
    uint32_t       display_width;
    uint32_t       display_height;
    uint16_t       output_refresh_hz;
    const char*    renderer_name;
    const char*    video_encoder_backend;
};

bool wd_server_cli_parse_u16(const char* text, uint16_t minimum, uint16_t maximum, uint16_t* value);
bool wd_server_cli_parse_size(const char* text, uint32_t maximum_width, uint32_t maximum_height, uint32_t* width, uint32_t* height);
bool wd_server_cli_parse_scale(const char* text, double minimum, double maximum, double* value);
bool wd_server_cli_parse_ipv4(const char* text, struct in_addr* address);
bool wd_server_cli_tile_grid_fits(uint32_t width, uint32_t height, uint16_t tile_width, uint16_t tile_height, uint32_t maximum_tiles);

enum wd_server_cli_parse_result wd_server_cli_parse_args(int argc, char* const* argv, struct wd_server_cli_options* options,
                                                         char* error_message, size_t error_message_size);

#ifdef __cplusplus
}
#endif
