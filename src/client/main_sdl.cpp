#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "client_net.hpp"
#include "sdl_viewer.hpp"
#include "wd_client.hpp"

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s <server_ipv4> <tcp_port> <client_udp_port>\n\n"
                 "Examples:\n"
                 "  %s 127.0.0.1 5000 6000\n",
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
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

    waydisplay::ClientState state;

    if (!waydisplay::client_connect(state,
                                    server_host,
                                    tcp_port,
                                    client_udp_port)) {
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
