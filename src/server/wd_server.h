#pragma once

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_server;

struct wd_server_config {
    uint16_t       tcp_port;
    struct in_addr listen_address;
    const char*    app_command;
    double         output_scale;
    uint32_t       display_width;
    uint32_t       display_height;
    uint16_t       tile_width;
    uint16_t       tile_height;
    bool           enable_xwayland;
    bool           enable_xdg_dialog;
    const char*    video_encoder_backend;
    uint8_t        tile_compression_benchmark_mode;
};

struct wd_server* wd_server_create(const struct wd_server_config* config);
void              wd_server_free(struct wd_server* server);
int               wd_server_run(struct wd_server* server);

#ifdef __cplusplus
}
#endif
