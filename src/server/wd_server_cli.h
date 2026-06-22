#pragma once

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wd_server_cli_parse_u16(const char* text, uint16_t minimum, uint16_t maximum,
                             uint16_t* value);
bool wd_server_cli_parse_size(const char* text, uint32_t maximum_width,
                              uint32_t maximum_height, uint32_t* width,
                              uint32_t* height);
bool wd_server_cli_parse_scale(const char* text, double minimum, double maximum,
                               double* value);
bool wd_server_cli_parse_ipv4(const char* text, struct in_addr* address);
bool wd_server_cli_tile_grid_fits(uint32_t width, uint32_t height, uint16_t tile_width,
                                  uint16_t tile_height, uint32_t maximum_tiles);

#ifdef __cplusplus
}
#endif
