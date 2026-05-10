#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "client_net.hpp"
#include "sdl_viewer.hpp"
#include "wd_client.hpp"

namespace {

    void usage(const char* argv0) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  %s <server_ipv4> <tcp_port> <client_udp_port> [options]\n\n"
                     "Options:\n"
                     "  --mode full                 Send every dirty compositor tick\n"
                     "  --mode partial              Cap send pass to target FPS, default 30\n"
                     "  --mode limited              Cap by tile budget, default 120 tiles/sec\n"
                     "  --fps <N>                   Target FPS for partial mode\n"
                     "  --max-tiles-per-sec <N>     Tile budget for limited mode\n\n"
                     "Examples:\n"
                     "  %s 127.0.0.1 5000 6000 --mode full\n"
                     "  %s 127.0.0.1 5000 6000 --mode partial --fps 30\n"
                     "  %s 127.0.0.1 5000 6000 --mode limited --max-tiles-per-sec 120\n",
                     argv0,
                     argv0,
                     argv0,
                     argv0);
    }

    bool parse_u16(const char* text, uint16_t& out) {
        if (!text || !*text) {
            return false;
        }

        char* end = nullptr;
        unsigned long value = std::strtoul(text, &end, 10);

        if (!end || *end != '\0' || value > 65535ul) {
            return false;
        }

        out = static_cast<uint16_t>(value);
        return true;
    }

    bool parse_u32(const char* text, uint32_t& out) {
        if (!text || !*text) {
            return false;
        }

        char* end = nullptr;
        unsigned long value = std::strtoul(text, &end, 10);

        if (!end || *end != '\0' || value > 0xfffffffful) {
            return false;
        }

        out = static_cast<uint32_t>(value);
        return true;
    }

    bool parse_mode(const char* text, waydisplay::ClientStreamMode& out) {
        if (std::strcmp(text, "full") == 0) {
            out = waydisplay::ClientStreamMode::Full;
            return true;
        }

        if (std::strcmp(text, "partial") == 0) {
            out = waydisplay::ClientStreamMode::Partial;
            return true;
        }

        if (std::strcmp(text, "limited") == 0) {
            out = waydisplay::ClientStreamMode::Limited;
            return true;
        }

        return false;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    const char* server_host = argv[1];

    uint16_t tcp_port = 0;
    uint16_t client_udp_port = 0;

    if (!parse_u16(argv[2], tcp_port) ||
        !parse_u16(argv[3], client_udp_port)) {
        usage(argv[0]);
    return 1;
        }

        waydisplay::ClientStreamConfig stream_config;

        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--mode") == 0) {
                if (i + 1 >= argc ||
                    !parse_mode(argv[i + 1], stream_config.mode)) {
                    usage(argv[0]);
                return 1;
                    }

                    ++i;
            } else if (std::strcmp(argv[i], "--fps") == 0) {
                if (i + 1 >= argc ||
                    !parse_u16(argv[i + 1], stream_config.target_fps) ||
                    stream_config.target_fps == 0) {
                    usage(argv[0]);
                return 1;
                    }

                    ++i;
            } else if (std::strcmp(argv[i], "--max-tiles-per-sec") == 0) {
                if (i + 1 >= argc ||
                    !parse_u32(argv[i + 1], stream_config.max_tiles_per_second) ||
                    stream_config.max_tiles_per_second == 0) {
                    usage(argv[0]);
                return 1;
                    }

                    ++i;
            } else {
                usage(argv[0]);
                return 1;
            }
        }

        waydisplay::ClientState state;

        if (!waydisplay::client_connect(state,
            server_host,
            tcp_port,
            client_udp_port,
            stream_config)) {
            waydisplay::client_disconnect(state);
        return 1;
            }

            if (!waydisplay::client_start_tcp_reader(state)) {
                waydisplay::client_disconnect(state);
                return 1;
            }

            const int rc = waydisplay::run_sdl_viewer(state);

            waydisplay::client_disconnect(state);

            return rc;
}
