#include "wd_server_cli.h"

#include <arpa/inet.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            exit(1);                                                                                \
        }                                                                                           \
    } while (0)

static void test_u16_parsing(void) {
    uint16_t value = 0;
    CHECK(wd_server_cli_parse_u16("0", 0, UINT16_MAX, &value) && value == 0);
    CHECK(wd_server_cli_parse_u16("65535", 0, UINT16_MAX, &value) && value == UINT16_MAX);
    CHECK(wd_server_cli_parse_u16("60", 1, 1000, &value) && value == 60);

    const char* invalid[] = {"", "abc", "-1", "+1", " 1", "1 ", "1x", "65536",
                             "70000", "18446744073709551616"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        CHECK(!wd_server_cli_parse_u16(invalid[i], 0, UINT16_MAX, &value));
    }
    CHECK(!wd_server_cli_parse_u16("0", 1, 1000, &value));
    CHECK(!wd_server_cli_parse_u16("1001", 1, 1000, &value));
}

static void test_size_parsing(void) {
    uint32_t width = 0;
    uint32_t height = 0;
    CHECK(wd_server_cli_parse_size("4096x2160", 4096, 2160, &width, &height));
    CHECK(width == 4096 && height == 2160);

    const char* invalid[] = {"",       "x",       "1x",      "x1",       "0x1",
                             "1x0",    "1X1",     "1x1junk", "1x1x1",    "-1x1",
                             "+1x1",   " 1x1",    "4097x1",  "1x2161",   "999999999999999999x1"};
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

    const char* invalid[] = {"", "abc", " 1", "1 ", "0.249", "8.001", "nan", "NaN",
                             "inf", "-inf", "1junk", "1e9999"};
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

    const char* invalid[] = {"", "localhost", " 127.0.0.1", "127.0.0.1 ",
                             "127.0.0.1:5000", "999.1.1.1", "::1"};
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

int main(void) {
    test_u16_parsing();
    test_size_parsing();
    test_scale_parsing();
    test_ipv4_parsing();
    test_tile_grid_overflow_checks();
    return 0;
}
