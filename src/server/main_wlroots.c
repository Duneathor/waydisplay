#include "wd_server.h"
#include "wd_tile_policy.h"
#include "wd_server_cli.h"

#include <arpa/inet.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [--listen 127.0.0.1] [--port 5000] [--scale 1.0] [--size 1664x1024] [--app <command>] "
            "[--tile-size 128x64|64x64|32x32|16x16] [--tile-compression auto|off|attempt|force] [--refresh-hz 60] "
            "[--renderer auto|gles2|vulkan|pixman] [--video-encoder auto|software|vaapi] [--vaapi-device /dev/dri/renderD128] "
            "[--xwayland|--no-xwayland] [--xdg-dialog|--no-xdg-dialog]\n\n"
            "Examples:\n"
            "  %s --port 5000 --app foot\n"
            "  %s --port 5000 --scale 1.25 --size 1366x768 --app foot\n"
            "  %s --port 5000 --app konsole\n"
            "  %s --port 5000 --app 'mpv /path/to/video.mp4'\n",
            argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char** argv) {
    const char* app_cmd         = "foot";
    uint16_t    tcp_port        = WD_DEFAULT_TCP_PORT;
    struct in_addr listen_address = {.s_addr = htonl(INADDR_LOOPBACK)};
    double      output_scale    = 1.0;
    uint32_t    display_width   = WD_DISPLAY_WIDTH;
    uint32_t    display_height  = WD_DISPLAY_HEIGHT;
    uint16_t    tile_width      = WD_TILE_WIDTH;
    uint16_t    tile_height     = WD_TILE_HEIGHT;
    uint16_t    output_refresh_hz = 60;
    bool        enable_xwayland = true;
    bool        enable_xdg_dialog = true;
    const char* renderer_name   = NULL;
    const char* video_encoder_backend = "auto";
    const char* vaapi_device = "/dev/dri/renderD128";
    uint8_t     tile_compression_benchmark_mode = WD_TILE_COMPRESSION_BENCH_AUTO;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--app") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            app_cmd = argv[++i];
        }
        else if (strcmp(argv[i], "--listen") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            if (!wd_server_cli_parse_ipv4(argv[++i], &listen_address))
            {
                fprintf(stderr, "Invalid --listen value: %s; expected an IPv4 address\n",
                        argv[i]);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--port") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            if (!wd_server_cli_parse_u16(argv[++i], 0, UINT16_MAX, &tcp_port))
            {
                fprintf(stderr, "Invalid --port value: %s; expected 0 through %u\n",
                        argv[i], UINT16_MAX);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--size") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            uint32_t width  = 0;
            uint32_t height = 0;
            if (!wd_server_cli_parse_size(argv[++i], WD_MAX_RENDER_WIDTH,
                                          WD_MAX_RENDER_HEIGHT, &width, &height))
            {
                fprintf(stderr, "Invalid --size value: %s; maximum render size is %ux%u\n",
                        argv[i], WD_MAX_RENDER_WIDTH, WD_MAX_RENDER_HEIGHT);
                usage(argv[0]);
                return 1;
            }

            if (!wd_server_cli_tile_grid_fits(width, height, tile_width, tile_height,
                                               UINT16_MAX))
            {
                fprintf(stderr, "Invalid --size value: %s creates too many tiles; max is %u tiles\n", argv[i], UINT16_MAX);
                usage(argv[0]);
                return 1;
            }

            display_width  = width;
            display_height = height;
        }
        else if (strcmp(argv[i], "--tile-size") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            uint32_t width  = 0;
            uint32_t height = 0;
            if (!wd_server_cli_parse_size(argv[++i], UINT16_MAX, UINT16_MAX,
                                          &width, &height))
            {
                fprintf(stderr, "Invalid --tile-size value: %s\n", argv[i]);
                usage(argv[0]);
                return 1;
            }

            const bool supported_tile_size = (width == 128u && height == 64u) || (width == 64u && height == 64u) ||
                                             (width == 32u && height == 32u) || (width == 16u && height == 16u);
            if (!supported_tile_size)
            {
                fprintf(stderr, "Invalid --tile-size value: %s; supported values are 128x64, 64x64, 32x32, and 16x16\n", argv[i]);
                usage(argv[0]);
                return 1;
            }

            uint64_t tile_bytes = (uint64_t)width * (uint64_t)height * WD_BYTES_PER_PIXEL;
            if (tile_bytes == 0 || tile_bytes > WD_TCP_MAX_PAYLOAD_SIZE)
            {
                fprintf(stderr, "Invalid --tile-size value: %s creates too large a tile buffer\n", argv[i]);
                usage(argv[0]);
                return 1;
            }

            if (!wd_server_cli_tile_grid_fits(display_width, display_height,
                                               (uint16_t)width, (uint16_t)height,
                                               UINT16_MAX))
            {
                fprintf(stderr, "Invalid --tile-size value: %s creates too many tiles for current --size; max is %u tiles\n", argv[i],
                        UINT16_MAX);
                usage(argv[0]);
                return 1;
            }

            tile_width  = (uint16_t)width;
            tile_height = (uint16_t)height;
        }
        else if (strcmp(argv[i], "--wan-tiles") == 0)
        {
            /* Smaller tiles reduce over-send for cursor/text/editor damage on
             * bandwidth-limited, high-latency links. */
            tile_width  = 64;
            tile_height = 64;
        }
        else if (strcmp(argv[i], "--tile-compression") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            if (!wd_tile_compression_benchmark_mode_parse(argv[++i], &tile_compression_benchmark_mode))
            {
                fprintf(stderr, "Invalid --tile-compression value: %s; expected auto, off, attempt, or force\n",
                        argv[i]);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--scale") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            if (!wd_server_cli_parse_scale(argv[++i], 0.25, 8.0, &output_scale))
            {
                fprintf(stderr, "Invalid --scale value: %s; expected 0.25 through 8.0\n",
                        argv[i]);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--refresh-hz") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            if (!wd_server_cli_parse_u16(argv[++i], 1, 1000, &output_refresh_hz))
            {
                fprintf(stderr, "Invalid --refresh-hz value: %s; expected 1 through 1000\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--renderer") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            renderer_name = argv[++i];
            if (strcmp(renderer_name, "auto") != 0 && strcmp(renderer_name, "gles2") != 0 &&
                strcmp(renderer_name, "vulkan") != 0 && strcmp(renderer_name, "pixman") != 0)
            {
                fprintf(stderr, "Invalid --renderer value: %s; expected auto, gles2, vulkan, or pixman\n", renderer_name);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--video-encoder") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }

            video_encoder_backend = argv[++i];
            if (strcmp(video_encoder_backend, "auto") != 0 &&
                strcmp(video_encoder_backend, "software") != 0 &&
                strcmp(video_encoder_backend, "vaapi") != 0)
            {
                fprintf(stderr, "Invalid --video-encoder value: %s; expected auto, software, or vaapi\n",
                        video_encoder_backend);
                usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--vaapi-device") == 0)
        {
            if (i + 1 >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            vaapi_device = argv[++i];
            if (vaapi_device[0] == '\0')
            {
                fprintf(stderr, "Invalid empty --vaapi-device value\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--xwayland") == 0)
        {
            enable_xwayland = true;
        }
        else if (strcmp(argv[i], "--no-xwayland") == 0)
        {
            enable_xwayland = false;
        }
        else if (strcmp(argv[i], "--xdg-dialog") == 0)
        {
            enable_xdg_dialog = true;
        }
        else if (strcmp(argv[i], "--no-xdg-dialog") == 0)
        {
            enable_xdg_dialog = false;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!wd_server_cli_tile_grid_fits(display_width, display_height, tile_width,
                                      tile_height, UINT16_MAX))
    {
        fprintf(stderr, "Invalid size/tile-size combination creates too many tiles; max is %u tiles\n", UINT16_MAX);
        usage(argv[0]);
        return 1;
    }

    if (renderer_name)
    {
        if (strcmp(renderer_name, "auto") == 0)
        {
            unsetenv("WLR_RENDERER");
        }
        else if (setenv("WLR_RENDERER", renderer_name, 1) != 0)
        {
            perror("setenv(WLR_RENDERER)");
            return 1;
        }
    }

    struct wd_server server;

    if (!wd_server_init(&server, tcp_port, listen_address, app_cmd, output_scale,
                        output_refresh_hz,
                        display_width, display_height, tile_width, tile_height, enable_xwayland,
                        enable_xdg_dialog, video_encoder_backend, vaapi_device))
    {
        wd_server_destroy(&server);
        return 1;
    }

    server.tile_compression_benchmark_mode = tile_compression_benchmark_mode;
    WD_LOG_INFO("tile compression policy: mode=%s",
                wd_tile_compression_benchmark_mode_name(tile_compression_benchmark_mode));

    int rc = wd_server_run(&server);

    wd_server_destroy(&server);

    return rc;
}
