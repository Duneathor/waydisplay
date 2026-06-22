#include "client_net.hpp"
#include "sdl_viewer.hpp"
#include "wd_client.hpp"
#include "waydisplay/wd_log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s <server_ipv4> <tcp_port> <client_udp_port> [options]\n\n"
                 "Options:\n"
                 "  --fps <N>                   Requested remote capture FPS cap, default 60\n"
                 "  --size <WxH>                 Request remote display size\n"
                 "  --rate-kib <N>               Cap adaptive UDP tile budget in KiB/s\n"
                 "  --no-vsync                   Create the SDL Vulkan renderer without present-vsync\n"
                 "  --no-audio                   Do not negotiate or configure the audio channel\n"
                 "  --video <auto|off|force>      Select video-mode policy, default auto\n"
                 "  --video-bitrate-kib <N>       Target video encoder bitrate in KiB/s, default link budget\n"
                 "  --video-min-dirty-percent <N> Dirty coverage needed for auto video mode, default 60\n"
                 "  --video-enter-seconds <N>     Seconds auto criteria must remain stable, default 3\n"
                 "  --video-exit-dirty-percent <N> Dirty coverage to leave video mode, default 30\n"
                 "  --video-exit-seconds <N>      Seconds exit criteria must remain stable, default 30\n"
                 "  --video-codec <auto|h264|h265> Select video codec, default h265\n"
                 "  --video-hwdecode <off|auto|vaapi> Optional video hardware decode, default auto\n"
                 "  --limited-rate-kib <N>       Deprecated alias for --rate-kib\n"
                 "  --wan                        Shorthand for --rate-kib %u\n\n"
                 "Examples:\n"
                 "  %s 127.0.0.1 5000 6000\n"
                 "  %s 127.0.0.1 5000 6000 --fps 60\n"
                 "  %s 127.0.0.1 5000 6000 --rate-kib 4096\n",
                 argv0, WD_CLIENT_WAN_RATE_KIB_PER_SECOND, argv0, argv0, argv0);
}

bool parse_u16(const char* text, uint16_t& out) {
    if (!text || !*text)
    {
        return false;
    }

    char*         end   = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);

    if (!end || *end != '\0' || value > 65535ul)
    {
        return false;
    }

    out = static_cast<uint16_t>(value);
    return true;
}

bool parse_u32(const char* text, uint32_t& out) {
    if (!text || !*text)
    {
        return false;
    }

    char*              end   = nullptr;
    unsigned long long value = std::strtoull(text, &end, 10);

    if (!end || *end != '\0' || value > 0xffffffffull)
    {
        return false;
    }

    out = static_cast<uint32_t>(value);
    return true;
}

bool parse_video_mode(const char* text, uint8_t& out) {
    if (!text)
    {
        return false;
    }

    if (std::strcmp(text, "auto") == 0)
    {
        out = WD_VIDEO_MODE_AUTO;
        return true;
    }
    if (std::strcmp(text, "off") == 0)
    {
        out = WD_VIDEO_MODE_OFF;
        return true;
    }
    if (std::strcmp(text, "force") == 0)
    {
        out = WD_VIDEO_MODE_FORCE;
        return true;
    }

    return false;
}

bool parse_video_codec(const char* text, uint32_t& out) {
    if (!text)
    {
        return false;
    }

    if (std::strcmp(text, "auto") == 0)
    {
        out = WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265;
        return true;
    }
    if (std::strcmp(text, "h264") == 0)
    {
        out = WD_VIDEO_CODEC_H264;
        return true;
    }
    if (std::strcmp(text, "h265") == 0 || std::strcmp(text, "hevc") == 0)
    {
        out = WD_VIDEO_CODEC_H265;
        return true;
    }

    return false;
}

bool parse_video_hwdecode_mode(const char* text, uint8_t& out) {
    if (!text)
    {
        return false;
    }

    if (std::strcmp(text, "auto") == 0)
    {
        out = WD_CLIENT_VIDEO_HWDECODE_AUTO;
        return true;
    }
    if (std::strcmp(text, "off") == 0)
    {
        out = WD_CLIENT_VIDEO_HWDECODE_OFF;
        return true;
    }
    if (std::strcmp(text, "vaapi") == 0)
    {
        out = WD_CLIENT_VIDEO_HWDECODE_VAAPI;
        return true;
    }

    return false;
}

bool parse_size(const char* text, uint16_t& width, uint16_t& height) {
    if (!text || !*text)
    {
        return false;
    }

    unsigned int parsed_width  = 0;
    unsigned int parsed_height = 0;

    if (std::sscanf(text, "%ux%u", &parsed_width, &parsed_height) != 2 || parsed_width == 0 || parsed_height == 0 ||
        parsed_width > 65535u || parsed_height > 65535u)
    {
        return false;
    }

    width  = static_cast<uint16_t>(parsed_width);
    height = static_cast<uint16_t>(parsed_height);
    return true;
}


} // namespace

int main(int argc, char** argv) {
    if (argc < 4)
    {
        usage(argv[0]);
        return 1;
    }

    const char* server_host = argv[1];

    uint16_t tcp_port        = 0;
    uint16_t client_udp_port = 0;

    if (!parse_u16(argv[2], tcp_port) || !parse_u16(argv[3], client_udp_port))
    {
        usage(argv[0]);
        return 1;
    }

    waydisplay::ClientStreamConfig stream_config;
    uint16_t                       desired_width  = 0;
    uint16_t                       desired_height = 0;

    for (int i = 4; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--mode") == 0)
        {
            WD_LOG_ERROR("--mode was removed; WayDisplay always uses adaptive max-rate streaming now.");
            return 1;
        }
        else if (std::strcmp(argv[i], "--fps") == 0)
        {
            if (i + 1 >= argc || !parse_u16(argv[i + 1], stream_config.target_fps) || stream_config.target_fps == 0)
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--size") == 0)
        {
            if (i + 1 >= argc || !parse_size(argv[i + 1], desired_width, desired_height) ||
                desired_width > WD_MAX_RENDER_WIDTH || desired_height > WD_MAX_RENDER_HEIGHT)
            {
                WD_LOG_ERROR("invalid --size; maximum render size is %ux%u",
                             WD_MAX_RENDER_WIDTH, WD_MAX_RENDER_HEIGHT);
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--rate-kib") == 0 || std::strcmp(argv[i], "--limited-rate-kib") == 0)
        {
            if (i + 1 >= argc || !parse_u32(argv[i + 1], stream_config.limited_udp_kib_per_second) ||
                stream_config.limited_udp_kib_per_second == 0)
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--no-vsync") == 0)
        {
            stream_config.disable_vsync = true;
        }
        else if (std::strcmp(argv[i], "--no-audio") == 0)
        {
            stream_config.disable_audio = true;
        }
        else if (std::strcmp(argv[i], "--video") == 0)
        {
            if (i + 1 >= argc || !parse_video_mode(argv[i + 1], stream_config.video_mode))
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-bitrate-kib") == 0)
        {
            if (i + 1 >= argc || !parse_u32(argv[i + 1], stream_config.video_bitrate_kib_per_second) ||
                stream_config.video_bitrate_kib_per_second == 0)
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-min-dirty-percent") == 0)
        {
            uint32_t percent = 0;
            if (i + 1 >= argc || !parse_u32(argv[i + 1], percent) ||
                percent > WD_VIDEO_MIN_DIRTY_PERCENT_MAX)
            {
                usage(argv[0]);
                return 1;
            }
            stream_config.video_min_dirty_percent = static_cast<uint8_t>(percent);

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-enter-seconds") == 0)
        {
            if (i + 1 >= argc || !parse_u16(argv[i + 1], stream_config.video_enter_seconds) ||
                stream_config.video_enter_seconds > WD_VIDEO_ENTER_SECONDS_MAX)
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-exit-dirty-percent") == 0)
        {
            uint32_t percent = 0;
            if (i + 1 >= argc || !parse_u32(argv[i + 1], percent) ||
                percent > WD_VIDEO_EXIT_DIRTY_PERCENT_MAX)
            {
                usage(argv[0]);
                return 1;
            }
            stream_config.video_exit_dirty_percent = static_cast<uint8_t>(percent);

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-exit-seconds") == 0)
        {
            if (i + 1 >= argc || !parse_u16(argv[i + 1], stream_config.video_exit_seconds) ||
                stream_config.video_exit_seconds > WD_VIDEO_EXIT_SECONDS_MAX)
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-codec") == 0)
        {
            if (i + 1 >= argc || !parse_video_codec(argv[i + 1], stream_config.video_codec_mask))
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--video-hwdecode") == 0)
        {
            if (i + 1 >= argc || !parse_video_hwdecode_mode(argv[i + 1], stream_config.video_hwdecode_mode))
            {
                usage(argv[0]);
                return 1;
            }

            ++i;
        }
        else if (std::strcmp(argv[i], "--wan") == 0)
        {
            stream_config.limited_udp_kib_per_second = WD_CLIENT_WAN_RATE_KIB_PER_SECOND;
        }
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    SDL_InitFlags sdl_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (!stream_config.disable_audio)
    {
        sdl_flags |= SDL_INIT_AUDIO;
    }
    if (!SDL_Init(sdl_flags))
    {
        WD_LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    waydisplay::ClientState state;

    if (!waydisplay::client_connect(state, server_host, tcp_port, client_udp_port, stream_config, desired_width, desired_height))
    {
        waydisplay::client_disconnect(state);
        SDL_Quit();
        return 1;
    }

    if (!waydisplay::client_start_tcp_reader(state))
    {
        waydisplay::client_disconnect(state);
        SDL_Quit();
        return 1;
    }

    if (!waydisplay::client_request_server_selections(state))
    {
        WD_LOG_WARN("failed to request initial server clipboard selections");
    }

    const int rc = waydisplay::run_sdl_viewer(state);

    waydisplay::client_disconnect(state);
    SDL_Quit();

    return rc;
}
