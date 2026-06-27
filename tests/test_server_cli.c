#include "waydisplay/wd_config.h"
#include "wd_server_cli.h"

#include <arpa/inet.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUTABLE_ARG(value) (char[]){value}

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                          \
            exit(1);                                                                                                                       \
        }                                                                                                                                  \
    } while (0)

static void test_u16_parsing(void) {
    uint16_t value = 0;
    CHECK(wd_server_cli_parse_u16("0", 0, UINT16_MAX, &value) && value == 0);
    CHECK(wd_server_cli_parse_u16("65535", 0, UINT16_MAX, &value) && value == UINT16_MAX);
    CHECK(wd_server_cli_parse_u16("60", 1, 1000, &value) && value == 60);

    const char* invalid[] = {"", "abc", "-1", "+1", " 1", "1 ", "1x", "65536", "70000", "18446744073709551616"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        CHECK(!wd_server_cli_parse_u16(invalid[i], 0, UINT16_MAX, &value));
    }
    CHECK(!wd_server_cli_parse_u16("0", 1, 1000, &value));
    CHECK(!wd_server_cli_parse_u16("1001", 1, 1000, &value));
}

static void test_size_parsing(void) {
    uint32_t width  = 0;
    uint32_t height = 0;
    CHECK(wd_server_cli_parse_size("4096x2160", 4096, 2160, &width, &height));
    CHECK(width == 4096 && height == 2160);

    const char* invalid[] = {
        "", "x", "1x", "x1", "0x1", "1x0", "1X1", "1x1junk", "1x1x1", "-1x1", "+1x1", " 1x1", "4097x1", "1x2161", "999999999999999999x1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        CHECK(!wd_server_cli_parse_size(invalid[i], 4096, 2160, &width, &height));
    }
}

static void test_scale_parsing(void) {
    double value = 0.0;
    CHECK(wd_server_cli_parse_scale("0.25", 0.25, 8.0, &value) && value == 0.25);
    CHECK(wd_server_cli_parse_scale("8", 0.25, 8.0, &value) && value == 8.0);
    CHECK(wd_server_cli_parse_scale("1e0", 0.25, 8.0, &value) && value == 1.0);

    const char* invalid[] = {"", "abc", " 1", "1 ", "0.249", "8.001", "nan", "NaN", "inf", "-inf", "1junk", "1e9999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        CHECK(!wd_server_cli_parse_scale(invalid[i], 0.25, 8.0, &value));
    }
}

static void test_ipv4_parsing(void) {
    struct in_addr address;
    CHECK(wd_server_cli_parse_ipv4("127.0.0.1", &address));
    CHECK(address.s_addr == htonl(INADDR_LOOPBACK));
    CHECK(wd_server_cli_parse_ipv4("0.0.0.0", &address));
    CHECK(address.s_addr == htonl(INADDR_ANY));

    const char* invalid[] = {"", "localhost", " 127.0.0.1", "127.0.0.1 ", "127.0.0.1:5000", "999.1.1.1", "::1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        CHECK(!wd_server_cli_parse_ipv4(invalid[i], &address));
    }
}

static void test_tile_grid_overflow_checks(void) {
    CHECK(wd_server_cli_tile_grid_fits(4096, 2160, 128, 64, UINT16_MAX));
    CHECK(!wd_server_cli_tile_grid_fits(4096, 4096, 16, 16, UINT16_MAX));
    CHECK(!wd_server_cli_tile_grid_fits(UINT32_MAX, UINT32_MAX, 1, 1, UINT32_MAX));
    CHECK(!wd_server_cli_tile_grid_fits(1, 1, 0, 1, UINT16_MAX));
}

static enum wd_server_cli_parse_result parse_args(size_t count, char** arguments, struct wd_server_cli_options* options) {
    char error_message[256] = {0};
    return wd_server_cli_parse_args((int)count, arguments, options, error_message, sizeof(error_message));
}

static void test_parse_defaults(void) {
    char*                        argv[] = {MUTABLE_ARG("server")};
    struct wd_server_cli_options options;
    CHECK(parse_args(sizeof(argv) / sizeof(argv[0]), argv, &options) == WD_SERVER_CLI_OK);
    CHECK(options.tcp_port == WD_DEFAULT_TCP_PORT);
    CHECK(options.display_width == WD_DISPLAY_WIDTH);
    CHECK(options.display_height == WD_DISPLAY_HEIGHT);
    CHECK(options.output_scale == WD_SERVER_DEFAULT_OUTPUT_SCALE);
    CHECK(strcmp(options.app_command, WD_SERVER_DEFAULT_APP_COMMAND) == 0);
    CHECK(strcmp(options.renderer_name, WD_SERVER_DEFAULT_RENDERER) == 0);
    CHECK(strcmp(options.video_encoder_backend, WD_SERVER_DEFAULT_VIDEO_ENCODER_BACKEND) == 0);
    struct in_addr expected_listen_address;
    CHECK(inet_pton(AF_INET, WD_SERVER_DEFAULT_LISTEN_IPV4, &expected_listen_address) == 1);
    CHECK(options.listen_address.s_addr == expected_listen_address.s_addr);
}

static void test_retained_arguments(void) {
    char* argv[] = {MUTABLE_ARG("server"),  MUTABLE_ARG("--listen"), MUTABLE_ARG("0.0.0.0"),      MUTABLE_ARG("--port"), MUTABLE_ARG("5500"),       MUTABLE_ARG("--app"),  MUTABLE_ARG("weston-terminal"), MUTABLE_ARG("--size"),  MUTABLE_ARG("1920x1080"),
                    MUTABLE_ARG("--scale"), MUTABLE_ARG("1.25"),
                    MUTABLE_ARG("--renderer"), MUTABLE_ARG("vulkan"), MUTABLE_ARG("--video-encoder"), MUTABLE_ARG("software")};
    struct wd_server_cli_options options;
    CHECK(parse_args(sizeof(argv) / sizeof(argv[0]), argv, &options) == WD_SERVER_CLI_OK);
    CHECK(options.listen_address.s_addr == htonl(INADDR_ANY));
    CHECK(options.tcp_port == 5500);
    CHECK(strcmp(options.app_command, "weston-terminal") == 0);
    CHECK(options.display_width == 1920 && options.display_height == 1080);
    CHECK(options.output_scale == 1.25);
    CHECK(strcmp(options.renderer_name, "vulkan") == 0);
    CHECK(strcmp(options.video_encoder_backend, "software") == 0);
}

static void test_removed_arguments(void) {
    char*        tile_size[]     = {MUTABLE_ARG("server"), MUTABLE_ARG("--tile-size"), MUTABLE_ARG("64x64")};
    char*        wan_tiles[]     = {MUTABLE_ARG("server"), MUTABLE_ARG("--wan-tiles")};
    char*        compression[]   = {MUTABLE_ARG("server"), MUTABLE_ARG("--tile-compression"), MUTABLE_ARG("force")};
    char*        xwayland[]      = {MUTABLE_ARG("server"), MUTABLE_ARG("--xwayland")};
    char*        no_xwayland[]   = {MUTABLE_ARG("server"), MUTABLE_ARG("--no-xwayland")};
    char*        xdg_dialog[]    = {MUTABLE_ARG("server"), MUTABLE_ARG("--xdg-dialog")};
    char*        no_xdg_dialog[] = {MUTABLE_ARG("server"), MUTABLE_ARG("--no-xdg-dialog")};
    char*        refresh_hz[]    = {MUTABLE_ARG("server"), MUTABLE_ARG("--refresh-hz"), MUTABLE_ARG("75")};
    char**       removed[]       = {tile_size, wan_tiles, compression, xwayland, no_xwayland, xdg_dialog, no_xdg_dialog, refresh_hz};
    const size_t counts[]        = {3, 2, 3, 2, 2, 2, 2, 3};

    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i)
    {
        struct wd_server_cli_options options;
        CHECK(parse_args(counts[i], removed[i], &options) == WD_SERVER_CLI_ERROR);
    }
}

static void test_help_and_errors(void) {
    char*                        help[]             = {MUTABLE_ARG("server"), MUTABLE_ARG("--help")};
    char*                        missing_app[]      = {MUTABLE_ARG("server"), MUTABLE_ARG("--app")};
    char*                        invalid_renderer[] = {MUTABLE_ARG("server"), MUTABLE_ARG("--renderer"), MUTABLE_ARG("metal")};
    char*                        invalid_size[]     = {MUTABLE_ARG("server"), MUTABLE_ARG("--size"), MUTABLE_ARG("4097x2160")};
    struct wd_server_cli_options options;
    CHECK(parse_args(2, help, &options) == WD_SERVER_CLI_HELP);
    CHECK(parse_args(2, missing_app, &options) == WD_SERVER_CLI_ERROR);
    CHECK(parse_args(3, invalid_renderer, &options) == WD_SERVER_CLI_ERROR);
    CHECK(parse_args(3, invalid_size, &options) == WD_SERVER_CLI_ERROR);
}

int main(void) {
    test_u16_parsing();
    test_size_parsing();
    test_scale_parsing();
    test_ipv4_parsing();
    test_tile_grid_overflow_checks();
    test_parse_defaults();
    test_retained_arguments();
    test_removed_arguments();
    test_help_and_errors();
    return 0;
}
