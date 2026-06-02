#include "wd_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [--port 5000] [--scale 1.0] [--size 1664x1024] [--app <command>] [--xwayland|--no-xwayland]\n\n"
            "Examples:\n"
            "  %s --port 5000 --app foot\n"
            "  %s --port 5000 --scale 1.25 --size 1366x768 --app foot\n"
            "  %s --port 5000 --app konsole\n"
            "  %s --port 5000 --app 'mpv /path/to/video.mp4'\n",
            argv0,
            argv0,
            argv0,
            argv0,
            argv0);
}

int main(int argc, char **argv) {
    const char *app_cmd = "foot";
    uint16_t tcp_port = 5000;
    double output_scale = 1.0;
    uint32_t display_width = WD_DISPLAY_WIDTH;
    uint32_t display_height = WD_DISPLAY_HEIGHT;
    bool enable_xwayland = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--app") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }

            app_cmd = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }

            tcp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }

            unsigned int width = 0;
            unsigned int height = 0;
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2 ||
                width == 0 || height == 0 ||
                width > UINT16_MAX || height > UINT16_MAX) {
                fprintf(stderr, "Invalid --size value: %s\n", argv[i]);
                usage(argv[0]);
                return 1;
            }

            const uint32_t tiles_x =
                (width + WD_TILE_WIDTH - 1u) / WD_TILE_WIDTH;
            const uint32_t tiles_y =
                (height + WD_TILE_HEIGHT - 1u) / WD_TILE_HEIGHT;

            if (tiles_x == 0 || tiles_y == 0 ||
                tiles_x * tiles_y > UINT16_MAX) {
                fprintf(stderr,
                        "Invalid --size value: %s creates too many tiles; max is %u tiles\n",
                        argv[i],
                        UINT16_MAX);
                usage(argv[0]);
                return 1;
            }

            display_width = width;
            display_height = height;
        } else if (strcmp(argv[i], "--scale") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }

            output_scale = atof(argv[++i]);
            if (output_scale < 0.25 || output_scale > 8.0) {
                fprintf(stderr, "Invalid --scale value: %.3f\n", output_scale);
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--xwayland") == 0) {
            enable_xwayland = true;
        } else if (strcmp(argv[i], "--no-xwayland") == 0) {
            enable_xwayland = false;
        } else if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
        return 0;
            } else {
                fprintf(stderr, "Unknown argument: %s\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
    }

    struct wd_server server;

    if (!wd_server_init(&server,
                        tcp_port,
                        app_cmd,
                        output_scale,
                        display_width,
                        display_height,
                        enable_xwayland)) {
        wd_server_destroy(&server);
        return 1;
    }

    int rc = wd_server_run(&server);

    wd_server_destroy(&server);

    return rc;
}
