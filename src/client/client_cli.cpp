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
    if (std::strcmp(text, "h265") == 0 || std::strcmp(text, "hevc") == 0)
    {
        value = WD_VIDEO_CODEC_H265;
        return true;
    }
    return false;
}

bool parse_video_hwdecode_mode(const char* text, uint8_t& value) {
    if (!text)
    {
        return false;
    }
    if (std::strcmp(text, "auto") == 0)
    {
        value = WD_CLIENT_VIDEO_HWDECODE_AUTO;
        return true;
    }
    if (std::strcmp(text, "off") == 0)
    {
        value = WD_CLIENT_VIDEO_HWDECODE_OFF;
        return true;
    }
    if (std::strcmp(text, "vaapi") == 0)
    {
        value = WD_CLIENT_VIDEO_HWDECODE_VAAPI;
        return true;
    }
    return false;
}

} // namespace

ClientCliParseResult client_cli_parse(int argc, const char* const* argv, ClientCliOptions& options, std::string* error_message) {
    options                     = ClientCliOptions{};
    options.target_fps          = WD_CLIENT_DEFAULT_TARGET_FPS;
    options.video_mode          = WD_VIDEO_MODE_AUTO;
    options.video_codec_mask    = WD_VIDEO_CODEC_H265;
    options.video_hwdecode_mode = WD_CLIENT_VIDEO_HWDECODE_AUTO;

    if (!argv || argc <= 0 || !argv[0])
    {
        set_error(error_message, "missing program name");
        return ClientCliParseResult::Error;
    }
    if (argc == 2 && argv[1] && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
    {
        return ClientCliParseResult::Help;
    }
    if (argc < 4 || !argv[1])
    {
        set_error(error_message, "expected server address and TCP/UDP ports");
        return ClientCliParseResult::Error;
    }

    options.server_host = argv[1];
    if (options.server_host.empty() || !parse_u16(argv[2], 1u, std::numeric_limits<uint16_t>::max(), options.tcp_port) ||
        !parse_u16(argv[3], 1u, std::numeric_limits<uint16_t>::max(), options.client_udp_port))
    {
        set_error(error_message, "invalid server address or port");
        return ClientCliParseResult::Error;
    }

    for (int i = 4; i < argc; ++i)
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
        if (std::strcmp(argument, "--fps") == 0)
        {
            if (++i >= argc || !parse_u16(argv[i], 1u, WD_MAX_REASONABLE_FPS, options.target_fps))
            {
                set_error(error_message, "invalid --fps value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--size") == 0)
        {
            if (++i >= argc || !parse_size(argv[i], options.desired_width, options.desired_height))
            {
                set_error(error_message, "invalid --size value");
                return ClientCliParseResult::Error;
            }
        }
        else if (std::strcmp(argument, "--rate-kib") == 0)
        {
            if (++i >= argc || !parse_u32(argv[i], 1u, std::numeric_limits<uint32_t>::max(), options.udp_rate_cap_kib_per_second))
            {
                set_error(error_message, "invalid --rate-kib value");
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
        else if (std::strcmp(argument, "--video") == 0)
        {
            if (++i >= argc || !parse_video_mode(argv[i], options.video_mode))
            {
                set_error(error_message, "invalid --video value");
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
        else if (std::strcmp(argument, "--video-hwdecode") == 0)
        {
            if (++i >= argc || !parse_video_hwdecode_mode(argv[i], options.video_hwdecode_mode))
            {
                set_error(error_message, "invalid --video-hwdecode value");
                return ClientCliParseResult::Error;
            }
        }
        else
        {
            set_error(error_message, "unknown client argument");
            return ClientCliParseResult::Error;
        }
    }

    return ClientCliParseResult::Ok;
}

} // namespace waydisplay
