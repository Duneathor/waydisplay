#include "wd_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [--port 5000] [--scale 1.0] [--app <command>]\n\n"
            "Examples:\n"
            "  %s --port 5000 --app foot\n"
            "  %s --port 5000 --scale 1.25 --app foot\n"
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

    if (!wd_server_init(&server, tcp_port, app_cmd, output_scale)) {
        wd_server_destroy(&server);
        return 1;
    }

    int rc = wd_server_run(&server);

    wd_server_destroy(&server);

    return rc;
}
