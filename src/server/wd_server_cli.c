#include "wd_server_cli.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static bool wd_server_cli_parse_decimal_span(const char* begin, const char* end,
                                             uint64_t maximum, uint64_t* value) {
    if (!begin || !end || !value || begin == end)
    {
        return false;
    }

    uint64_t parsed = 0;
    for (const char* cursor = begin; cursor != end; ++cursor)
    {
        const unsigned char character = (unsigned char)*cursor;
        if (!isdigit(character))
        {
            return false;
        }
        const uint64_t digit = (uint64_t)(character - (unsigned char)'0');
        if (digit > maximum || parsed > (maximum - digit) / 10u)
        {
            return false;
        }
        parsed = parsed * 10u + digit;
    }

    *value = parsed;
    return true;
}

bool wd_server_cli_parse_u16(const char* text, uint16_t minimum, uint16_t maximum,
                             uint16_t* value) {
    if (!text || !value || minimum > maximum)
    {
        return false;
    }

    const char* end = text + strlen(text);
    uint64_t parsed = 0;
    if (!wd_server_cli_parse_decimal_span(text, end, maximum, &parsed) || parsed < minimum)
    {
        return false;
    }

    *value = (uint16_t)parsed;
    return true;
}

bool wd_server_cli_parse_size(const char* text, uint32_t maximum_width,
                              uint32_t maximum_height, uint32_t* width,
                              uint32_t* height) {
    if (!text || !width || !height || maximum_width == 0 || maximum_height == 0)
    {
        return false;
    }

    const char* separator = strchr(text, 'x');
    if (!separator || strchr(separator + 1, 'x'))
    {
        return false;
    }

    const char* end = text + strlen(text);
    uint64_t parsed_width = 0;
    uint64_t parsed_height = 0;
    if (!wd_server_cli_parse_decimal_span(text, separator, maximum_width, &parsed_width) ||
        !wd_server_cli_parse_decimal_span(separator + 1, end, maximum_height, &parsed_height) ||
        parsed_width == 0 || parsed_height == 0)
    {
        return false;
    }

    *width = (uint32_t)parsed_width;
    *height = (uint32_t)parsed_height;
    return true;
}

bool wd_server_cli_parse_scale(const char* text, double minimum, double maximum,
                               double* value) {
    if (!text || !value || !isfinite(minimum) || !isfinite(maximum) || minimum > maximum ||
        text[0] == '\0' || isspace((unsigned char)text[0]))
    {
        return false;
    }

    errno = 0;
    char* end = NULL;
    const double parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || !end || *end != '\0' || !isfinite(parsed) ||
        parsed < minimum || parsed > maximum)
    {
        return false;
    }

    *value = parsed;
    return true;
}


bool wd_server_cli_parse_ipv4(const char* text, struct in_addr* address) {
    if (!text || !address || text[0] == '\0' || isspace((unsigned char)text[0]))
    {
        return false;
    }

    struct in_addr parsed;
    if (inet_pton(AF_INET, text, &parsed) != 1)
    {
        return false;
    }

    *address = parsed;
    return true;
}

bool wd_server_cli_tile_grid_fits(uint32_t width, uint32_t height, uint16_t tile_width,
                                  uint16_t tile_height, uint32_t maximum_tiles) {
    if (width == 0 || height == 0 || tile_width == 0 || tile_height == 0 ||
        maximum_tiles == 0)
    {
        return false;
    }

    const uint64_t tiles_x = ((uint64_t)width + tile_width - 1u) / tile_width;
    const uint64_t tiles_y = ((uint64_t)height + tile_height - 1u) / tile_height;
    return tiles_x != 0 && tiles_y != 0 && tiles_x <= maximum_tiles &&
           tiles_y <= maximum_tiles / tiles_x;
}
