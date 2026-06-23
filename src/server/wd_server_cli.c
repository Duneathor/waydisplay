#include "wd_server_cli.h"

#include "waydisplay/wd_config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool wd_server_cli_parse_decimal_span(const char* begin, const char* end, uint64_t maximum, uint64_t* value) {
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

bool wd_server_cli_parse_u16(const char* text, uint16_t minimum, uint16_t maximum, uint16_t* value) {
    if (!text || !value || minimum > maximum)
    {
        return false;
    }

    const char* end    = text + strlen(text);
    uint64_t    parsed = 0;
    if (!wd_server_cli_parse_decimal_span(text, end, maximum, &parsed) || parsed < minimum)
    {
        return false;
    }

    *value = (uint16_t)parsed;
    return true;
}

bool wd_server_cli_parse_size(const char* text, uint32_t maximum_width, uint32_t maximum_height, uint32_t* width, uint32_t* height) {
    if (!text || !width || !height || maximum_width == 0 || maximum_height == 0)
    {
        return false;
    }

    const char* separator = strchr(text, 'x');
    if (!separator || strchr(separator + 1, 'x'))
    {
        return false;
    }

    const char* end           = text + strlen(text);
    uint64_t    parsed_width  = 0;
    uint64_t    parsed_height = 0;
    if (!wd_server_cli_parse_decimal_span(text, separator, maximum_width, &parsed_width) ||
        !wd_server_cli_parse_decimal_span(separator + 1, end, maximum_height, &parsed_height) || parsed_width == 0 || parsed_height == 0)
    {
        return false;
    }

    *width  = (uint32_t)parsed_width;
    *height = (uint32_t)parsed_height;
    return true;
}

bool wd_server_cli_parse_scale(const char* text, double minimum, double maximum, double* value) {
    if (!text || !value || !isfinite(minimum) || !isfinite(maximum) || minimum > maximum || text[0] == '\0' ||
        isspace((unsigned char)text[0]))
    {
        return false;
    }

    errno               = 0;
    char*        end    = NULL;
    const double parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || !end || *end != '\0' || !isfinite(parsed) || parsed < minimum || parsed > maximum)
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

bool wd_server_cli_tile_grid_fits(uint32_t width, uint32_t height, uint16_t tile_width, uint16_t tile_height, uint32_t maximum_tiles) {
    if (width == 0 || height == 0 || tile_width == 0 || tile_height == 0 || maximum_tiles == 0)
    {
        return false;
    }

    const uint64_t tiles_x = ((uint64_t)width + tile_width - 1u) / tile_width;
    const uint64_t tiles_y = ((uint64_t)height + tile_height - 1u) / tile_height;
    return tiles_x != 0 && tiles_y != 0 && tiles_x <= maximum_tiles && tiles_y <= maximum_tiles / tiles_x;
}

static void wd_server_cli_set_error(char* error_message, size_t error_message_size, const char* format, ...) {
    if (!error_message || error_message_size == 0)
    {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error_message, error_message_size, format, arguments);
    va_end(arguments);
}

static bool wd_server_cli_renderer_is_valid(const char* name) {
    return name && (strcmp(name, "auto") == 0 || strcmp(name, "gles2") == 0 || strcmp(name, "vulkan") == 0 || strcmp(name, "pixman") == 0);
}

static bool wd_server_cli_video_encoder_is_valid(const char* name) {
    return name && (strcmp(name, "auto") == 0 || strcmp(name, "software") == 0 || strcmp(name, "vaapi") == 0);
}

enum wd_server_cli_parse_result wd_server_cli_parse_args(int argc, char* const* argv, struct wd_server_cli_options* options,
                                                         char* error_message, size_t error_message_size) {
    if (!argv || argc <= 0 || !argv[0] || !options)
    {
        wd_server_cli_set_error(error_message, error_message_size, "invalid command-line storage");
        return WD_SERVER_CLI_ERROR;
    }

    memset(options, 0, sizeof(*options));
    options->app_command           = WD_SERVER_DEFAULT_APP_COMMAND;
    options->tcp_port              = WD_DEFAULT_TCP_PORT;
    options->output_scale          = WD_SERVER_DEFAULT_OUTPUT_SCALE;
    options->display_width         = WD_DISPLAY_WIDTH;
    options->display_height        = WD_DISPLAY_HEIGHT;
    options->output_refresh_hz     = WD_SERVER_DEFAULT_REFRESH_HZ;
    options->renderer_name         = WD_SERVER_DEFAULT_RENDERER;
    options->video_encoder_backend = WD_SERVER_DEFAULT_VIDEO_ENCODER_BACKEND;
    if (!wd_server_cli_parse_ipv4(WD_SERVER_DEFAULT_LISTEN_IPV4, &options->listen_address))
    {
        wd_server_cli_set_error(error_message, error_message_size, "invalid compiled default listen address: %s",
                                WD_SERVER_DEFAULT_LISTEN_IPV4);
        return WD_SERVER_CLI_ERROR;
    }

    for (int i = 1; i < argc; ++i)
    {
        const char* argument = argv[i];
        if (!argument)
        {
            wd_server_cli_set_error(error_message, error_message_size, "null command-line argument");
            return WD_SERVER_CLI_ERROR;
        }
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0)
        {
            return WD_SERVER_CLI_HELP;
        }
        if (strcmp(argument, "--app") == 0)
        {
            if (++i >= argc || !argv[i] || argv[i][0] == '\0')
            {
                wd_server_cli_set_error(error_message, error_message_size, "--app requires a nonempty command");
                return WD_SERVER_CLI_ERROR;
            }
            options->app_command = argv[i];
        }
        else if (strcmp(argument, "--listen") == 0)
        {
            if (++i >= argc || !wd_server_cli_parse_ipv4(argv[i], &options->listen_address))
            {
                wd_server_cli_set_error(error_message, error_message_size, "invalid --listen value; expected an IPv4 address");
                return WD_SERVER_CLI_ERROR;
            }
        }
        else if (strcmp(argument, "--port") == 0)
        {
            if (++i >= argc || !wd_server_cli_parse_u16(argv[i], 0, UINT16_MAX, &options->tcp_port))
            {
                wd_server_cli_set_error(error_message, error_message_size, "invalid --port value; expected 0 through %u", UINT16_MAX);
                return WD_SERVER_CLI_ERROR;
            }
        }
        else if (strcmp(argument, "--size") == 0)
        {
            uint32_t width  = 0;
            uint32_t height = 0;
            if (++i >= argc || !wd_server_cli_parse_size(argv[i], WD_MAX_RENDER_WIDTH, WD_MAX_RENDER_HEIGHT, &width, &height) ||
                !wd_server_cli_tile_grid_fits(width, height, WD_TILE_WIDTH, WD_TILE_HEIGHT, UINT16_MAX))
            {
                wd_server_cli_set_error(error_message, error_message_size,
                                        "invalid --size value; maximum is %ux%u and the configured tile grid must fit", WD_MAX_RENDER_WIDTH,
                                        WD_MAX_RENDER_HEIGHT);
                return WD_SERVER_CLI_ERROR;
            }
            options->display_width  = width;
            options->display_height = height;
        }
        else if (strcmp(argument, "--scale") == 0)
        {
            if (++i >= argc ||
                !wd_server_cli_parse_scale(argv[i], WD_SERVER_MIN_OUTPUT_SCALE, WD_SERVER_MAX_OUTPUT_SCALE, &options->output_scale))
            {
                wd_server_cli_set_error(error_message, error_message_size, "invalid --scale value; expected %.2f through %.2f",
                                        WD_SERVER_MIN_OUTPUT_SCALE, WD_SERVER_MAX_OUTPUT_SCALE);
                return WD_SERVER_CLI_ERROR;
            }
        }
        else if (strcmp(argument, "--refresh-hz") == 0)
        {
            if (++i >= argc ||
                !wd_server_cli_parse_u16(argv[i], WD_SERVER_MIN_REFRESH_HZ, WD_SERVER_MAX_REFRESH_HZ, &options->output_refresh_hz))
            {
                wd_server_cli_set_error(error_message, error_message_size, "invalid --refresh-hz value; expected %u through %u",
                                        WD_SERVER_MIN_REFRESH_HZ, WD_SERVER_MAX_REFRESH_HZ);
                return WD_SERVER_CLI_ERROR;
            }
        }
        else if (strcmp(argument, "--renderer") == 0)
        {
            if (++i >= argc || !wd_server_cli_renderer_is_valid(argv[i]))
            {
                wd_server_cli_set_error(error_message, error_message_size,
                                        "invalid --renderer value; expected auto, gles2, vulkan, or pixman");
                return WD_SERVER_CLI_ERROR;
            }
            options->renderer_name = argv[i];
        }
        else if (strcmp(argument, "--video-encoder") == 0)
        {
            if (++i >= argc || !wd_server_cli_video_encoder_is_valid(argv[i]))
            {
                wd_server_cli_set_error(error_message, error_message_size,
                                        "invalid --video-encoder value; expected auto, software, or vaapi");
                return WD_SERVER_CLI_ERROR;
            }
            options->video_encoder_backend = argv[i];
        }
        else
        {
            wd_server_cli_set_error(error_message, error_message_size, "unknown server argument: %s", argument);
            return WD_SERVER_CLI_ERROR;
        }
    }

    return WD_SERVER_CLI_OK;
}
