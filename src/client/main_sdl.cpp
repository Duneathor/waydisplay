#include "client_net.hpp"
#include "sdl_viewer.hpp"
#include "wd_client.hpp"
#include "waydisplay/wd_log.h"

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
                 "  --fps <N>                   Target adaptive render/discovery FPS, default 30\n"
                 "  --size <WxH>                 Request remote display size\n"
                 "  --rate-kib <N>               Cap adaptive UDP tile budget in KiB/s\n"
                 "  --no-vsync                   Create the SDL renderer without present-vsync\n"
                 "  --limited-rate-kib <N>       Deprecated alias for --rate-kib\n"
                 "  --wan                        Shorthand for --rate-kib 4096\n\n"
                 "Examples:\n"
                 "  %s 127.0.0.1 5000 6000\n"
                 "  %s 127.0.0.1 5000 6000 --fps 30\n"
                 "  %s 127.0.0.1 5000 6000 --rate-kib 4096\n",
                 argv0, argv0, argv0, argv0);
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
            if (i + 1 >= argc || !parse_size(argv[i + 1], desired_width, desired_height))
            {
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
        else if (std::strcmp(argv[i], "--wan") == 0)
        {
            stream_config.limited_udp_kib_per_second = 4096;
        }
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    waydisplay::ClientState state;

    if (!waydisplay::client_connect(state, server_host, tcp_port, client_udp_port, stream_config, desired_width, desired_height))
    {
        waydisplay::client_disconnect(state);
        return 1;
    }

    if (!waydisplay::client_start_tcp_reader(state))
    {
        waydisplay::client_disconnect(state);
        return 1;
    }

    const int rc = waydisplay::run_sdl_viewer(state);

    waydisplay::client_disconnect(state);

    return rc;
}
