#include "client_cli.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace waydisplay {
namespace {

void set_error(std::string* error_message, const char* message) {
    if (error_message)
    {
        *error_message = message ? message : "invalid command line";
    }
}

bool parse_decimal(const char* text, uint64_t maximum, uint64_t& value) {
    if (!text || text[0] == '\0')
    {
        return false;
    }

    uint64_t parsed = 0;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text); *cursor != '\0'; ++cursor)
    {
        if (*cursor < static_cast<unsigned char>('0') || *cursor > static_cast<unsigned char>('9'))
        {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(*cursor - static_cast<unsigned char>('0'));
        if (parsed > (maximum - digit) / 10u)
        {
            return false;
        }
        parsed = parsed * 10u + digit;
    }

    value = parsed;
    return true;
}

bool parse_u16(const char* text, uint16_t minimum, uint16_t maximum, uint16_t& value) {
    uint64_t parsed = 0;
    if (!parse_decimal(text, maximum, parsed) || parsed < minimum)
    {
        return false;
    }
    value = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_u32(const char* text, uint32_t minimum, uint32_t maximum, uint32_t& value) {
    uint64_t parsed = 0;
    if (!parse_decimal(text, maximum, parsed) || parsed < minimum)
    {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_size(const char* text, uint16_t& width, uint16_t& height) {
    if (!text)
    {
        return false;
    }

    const char* separator = std::strchr(text, 'x');
    if (!separator || separator == text || separator[1] == '\0' || std::strchr(separator + 1, 'x'))
    {
        return false;
    }

    uint64_t parsed_width  = 0;
    uint64_t parsed_height = 0;
    uint64_t multiplier    = 1;
    for (const char* cursor = separator; cursor != text;)
    {
        --cursor;
        const unsigned char character = static_cast<unsigned char>(*cursor);
        if (character < static_cast<unsigned char>('0') || character > static_cast<unsigned char>('9'))
        {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(character - static_cast<unsigned char>('0'));
        if (digit > (WD_MAX_RENDER_WIDTH - parsed_width) / multiplier)
        {
            return false;
        }
        parsed_width += digit * multiplier;
        if (cursor != text)
        {
            if (multiplier > WD_MAX_RENDER_WIDTH / 10u)
            {
                return false;
            }
            multiplier *= 10u;
        }
    }

    multiplier      = 1;
    const char* end = text + std::strlen(text);
    for (const char* cursor = end; cursor != separator + 1;)
    {
        --cursor;
        const unsigned char character = static_cast<unsigned char>(*cursor);
        if (character < static_cast<unsigned char>('0') || character > static_cast<unsigned char>('9'))
        {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(character - static_cast<unsigned char>('0'));
        if (digit > (WD_MAX_RENDER_HEIGHT - parsed_height) / multiplier)
        {
            return false;
        }
        parsed_height += digit * multiplier;
        if (cursor != separator + 1)
        {
            if (multiplier > WD_MAX_RENDER_HEIGHT / 10u)
            {
                return false;
            }
            multiplier *= 10u;
        }
    }

    if (parsed_width == 0 || parsed_height == 0 || parsed_width > std::numeric_limits<uint16_t>::max() ||
        parsed_height > std::numeric_limits<uint16_t>::max())
    {
        return false;
    }

    width  = static_cast<uint16_t>(parsed_width);
    height = static_cast<uint16_t>(parsed_height);
    return true;
}

bool parse_video_mode(const char* text, uint8_t& value) {
    if (!text)
    {
        return false;
    }
    if (std::strcmp(text, "auto") == 0)
    {
        value = WD_VIDEO_MODE_AUTO;
        return true;
    }
    if (std::strcmp(text, "off") == 0)
    {
        value = WD_VIDEO_MODE_OFF;
        return true;
    }
    if (std::strcmp(text, "force") == 0)
    {
        value = WD_VIDEO_MODE_FORCE;
        return true;
    }
    return false;
}

bool parse_video_codec(const char* text, uint32_t& value) {
    if (!text)
    {
        return false;
    }
    if (std::strcmp(text, "auto") == 0)
    {
        value = WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265;
        return true;
    }
    if (std::strcmp(text, "h264") == 0)
    {
        value = WD_VIDEO_CODEC_H264;
        return true;
    }
    if (std::strcmp(text, "av1") == 0)
    {
        value = WD_VIDEO_CODEC_AV1;
        return true;
    }
    if (std::strcmp(text, "h265") == 0 || std::strcmp(text, "hevc") == 0)
    {
        value = WD_VIDEO_CODEC_H265;
        return true;
    }
    return false;
}

bool parse_video_decoder_mode(const char* text, uint8_t& value) {
    if (!text)
    {
        return false;
    }
    if (std::strcmp(text, "auto") == 0)
    {
        value = WD_CLIENT_VIDEO_DECODER_AUTO;
        return true;
    }
    if (std::strcmp(text, "off") == 0)
    {
        value = WD_CLIENT_VIDEO_DECODER_OFF;
        return true;
    }
    if (std::strcmp(text, "vaapi") == 0)
    {
        value = WD_CLIENT_VIDEO_DECODER_VAAPI;
        return true;
    }
    if (std::strcmp(text, "software") == 0)
    {
        value = WD_CLIENT_VIDEO_DECODER_SOFTWARE;
        return true;
    }
    return false;
}

} // namespace

ClientCliParseResult client_cli_parse(int argc, const char* const* argv, ClientCliOptions& options, std::string* error_message) {
    options                       = ClientCliOptions{};
    options.tcp_port              = WD_DEFAULT_TCP_PORT;
    options.client_udp_port       = WD_CLIENT_DEFAULT_UDP_PORT;
    options.requested_session_fps = WD_CLIENT_DEFAULT_SESSION_FPS;
    options.video_mode           = WD_VIDEO_MODE_AUTO;
    options.video_codec_mask     = WD_VIDEO_CODEC_H265;
    options.video_decoder_mode   = WD_CLIENT_VIDEO_DECODER_AUTO;

    if (!argv || argc <= 0 || !argv[0])
    {
        set_error(error_message, "missing program name");
        return ClientCliParseResult::Error;
    }
    if (argc == 2 && argv[1] && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        return ClientCliParseResult::Help;
    }
    /* One required address, followed by optional positional TCP and UDP ports.
     * Options can follow the address or an explicitly provided port. */
    unsigned positionals = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* argument = argv[i];
        if (!argument)
        {
            set_error(error_message, "null command-line argument");
            return ClientCliParseResult::Error;
        }
        if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0)
        {
            return ClientCliParseResult::Help;
        }
        if (std::strcmp(argument, "--verbose") == 0 || std::strcmp(argument, "-v") == 0)
        {
            options.verbose = true;
        }
        else if (std::strcmp(argument, "--session-fps") == 0)
        {
            if (++i >= argc || !parse_u16(argv[i], 1u, WD_MAX_SESSION_FPS, options.requested_session_fps))
            {
                set_error(error_message, "invalid --session-fps value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--display-size") == 0)
        {
            if (++i >= argc || !parse_size(argv[i], options.desired_width, options.desired_height))
            {
                set_error(error_message, "invalid --display-size value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--link-cap-kib-per-sec") == 0)
        {
            if (++i >= argc || !parse_u32(argv[i], 1u, std::numeric_limits<uint32_t>::max(), options.link_cap_kib_per_second))
            {
                set_error(error_message, "invalid --link-cap-kib-per-sec value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--no-vsync") == 0)
        {
            options.disable_vsync = true;
        }
        else if (std::strcmp(argument, "--no-audio") == 0)
        {
            options.disable_audio = true;
        }
        else if (std::strcmp(argument, "--video-mode") == 0)
        {
            if (++i >= argc || !parse_video_mode(argv[i], options.video_mode))
            {
                set_error(error_message, "invalid --video-mode value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--video-codec") == 0)
        {
            if (++i >= argc || !parse_video_codec(argv[i], options.video_codec_mask))
            {
                set_error(error_message, "invalid --video-codec value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--video-decoder") == 0)
        {
            if (++i >= argc || !parse_video_decoder_mode(argv[i], options.video_decoder_mode))
            {
                set_error(error_message, "invalid --video-decoder value; expected off, auto, vaapi, or software");
                return ClientCliParseResult::Error;
            }
        }
        else if (argument[0] == '-')
        {
            set_error(error_message, "unknown client option");
            return ClientCliParseResult::Error;
        }
        else if (positionals == 0u && argument[0] != '\0')
        {
            options.server_host = argument;
            ++positionals;
        }
        else if (positionals == 1u && parse_u16(argument, 1u, UINT16_MAX, options.tcp_port))
        {
            ++positionals;
        }
        else if (positionals == 2u && parse_u16(argument, 1u, UINT16_MAX, options.client_udp_port))
        {
            ++positionals;
        }
        else
        {
            set_error(error_message, "expected <server_ipv4> [tcp_port [client_udp_port]] with valid ports");
            return ClientCliParseResult::Error;
        }
    }

    if (options.server_host.empty())
    {
        set_error(error_message, "missing server address");
        return ClientCliParseResult::Error;
    }
    return ClientCliParseResult::Ok;
}

} // namespace waydisplay
