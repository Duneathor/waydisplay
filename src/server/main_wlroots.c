#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "wd_server.h"
#include "wd_server_cli.h"

#include "waydisplay/wd_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [options]\n\n"
            "Options:\n"
            "  --listen <IPv4>                 Bind address, default %s\n"
            "  --port <N>                      TCP control port, default %u\n"
            "  --app <command>                 Command launched in the compositor, default %s\n"
            "  --size <WxH>                    Virtual output size, default %ux%u\n"
            "  --scale <N>                     Output scale, default %.2f\n"
            "  --renderer <auto|gles2|vulkan|pixman>\n"
            "  --video-encoder <auto|software|vaapi>\n"
            "  --help, -h                      Show this help\n\n"
            "Tile policy, Xwayland, and xdg-dialog behavior are configured in wd_config.h.\n\n"
            "Examples:\n"
            "  %s --listen 0.0.0.0 --port %u --app %s\n"
            "  %s --scale 1.25 --size 1366x768 --app konsole\n",
            argv0, WD_SERVER_DEFAULT_LISTEN_IPV4, WD_DEFAULT_TCP_PORT, WD_SERVER_DEFAULT_APP_COMMAND, WD_DISPLAY_WIDTH, WD_DISPLAY_HEIGHT,
            WD_SERVER_DEFAULT_OUTPUT_SCALE, argv0, WD_DEFAULT_TCP_PORT, WD_SERVER_DEFAULT_APP_COMMAND, argv0);
}

int main(int argc, char** argv) {
    struct wd_server_cli_options          options;
    char                                  error_message[256] = {0};
    const enum wd_server_cli_parse_result parse_result =
        wd_server_cli_parse_args(argc, argv, &options, error_message, sizeof(error_message));
    if (parse_result == WD_SERVER_CLI_HELP)
    {
        usage(argv[0]);
        return 0;
    }
    if (parse_result != WD_SERVER_CLI_OK)
    {
        if (error_message[0] != '\0')
        {
            fprintf(stderr, "%s\n", error_message);
        }
        usage(argv[0]);
        return 1;
    }

    if (strcmp(options.renderer_name, "auto") == 0)
    {
        unsetenv("WLR_RENDERER");
    }
    else if (setenv("WLR_RENDERER", options.renderer_name, 1) != 0)
    {
        perror("setenv(WLR_RENDERER)");
        return 1;
    }

    const struct wd_server_config config = {
        .tcp_port                       = options.tcp_port,
        .listen_address                 = options.listen_address,
        .app_command                    = options.app_command,
        .output_scale                   = options.output_scale,
        .display_width                  = options.display_width,
        .display_height                 = options.display_height,
        .tile_width                     = WD_TILE_WIDTH,
        .tile_height                    = WD_TILE_HEIGHT,
        .enable_xwayland                = WD_SERVER_DEFAULT_ENABLE_XWAYLAND != 0,
        .enable_xdg_dialog              = WD_SERVER_DEFAULT_ENABLE_XDG_DIALOG != 0,
        .video_encoder_backend          = options.video_encoder_backend,
        .tile_compression_benchmark_mode = (uint8_t)WD_SERVER_TILE_COMPRESSION_BENCHMARK_MODE_DEFAULT,
    };
    struct wd_server* server = wd_server_create(&config);
    if (!server)
    {
        return 1;
    }

    const int rc = wd_server_run(server);
    wd_server_free(server);
    return rc;
}
