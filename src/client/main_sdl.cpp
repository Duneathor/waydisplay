#include "client_cli.hpp"
#include "client_net.hpp"
#include "sdl_viewer.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "client_state.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s <server_ipv4> [tcp_port [client_udp_port]] [options]\n\n"
                 "Defaults: TCP %u, local UDP %u\n\n"
                 "Options:\n"
                 "  -v, --verbose                 Enable diagnostic output (default: errors only)\n"
                 "  --session-fps <N>              Session refresh/present FPS ceiling, default %u\n"
                 "  --display-size <WxH>          Request remote display size\n"
                 "  --link-cap-kib-per-sec <N>   Cap estimated safe link budget (KiB/s)\n"
                 "  --no-vsync                    Disable SDL present-vsync\n"
                 "  --no-audio                    Disable audio negotiation and playback\n"
                 "  --video-mode <auto|off|force>  Select coarse video-mode policy, default auto\n"
                 "  --video-codec <auto|h264|h265|av1> Select acceptable video codecs, default h265\n"
                 "  --video-decoder <off|auto|vaapi|software> Select decoder backend, default auto\n"
                 "  --help, -h                    Show this help\n\n"
                 "Detailed stream thresholds and codec policy are configured in wd_config.h.\n\n"
                 "Examples:\n"
                 "  %s 127.0.0.1\n"
                 "  %s 192.168.1.50 5500 6500 --session-fps 60 --link-cap-kib-per-sec 4096 -v\n",
                 argv0, WD_DEFAULT_TCP_PORT, WD_CLIENT_DEFAULT_UDP_PORT, WD_CLIENT_DEFAULT_SESSION_FPS, argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    waydisplay::ClientCliOptions   cli_options;
    std::string                    error_message;
    const std::vector<const char*> arguments(argv, argv + argc);
    const auto                     parse_result = waydisplay::client_cli_parse(argc, arguments.data(), cli_options, &error_message);
    if (parse_result == waydisplay::ClientCliParseResult::Help)
    {
        usage(argv[0]);
        return 0;
    }
    if (parse_result != waydisplay::ClientCliParseResult::Ok)
    {
        if (!error_message.empty())
        {
            WD_LOG_ERROR("%s", error_message.c_str());
        }
        usage(argv[0]);
        return 1;
    }

    wd_log_set_verbose(cli_options.verbose);

    waydisplay::ClientStreamConfig stream_config;
    stream_config.requested_session_fps   = cli_options.requested_session_fps;
    stream_config.link_cap_kib_per_second = cli_options.link_cap_kib_per_second;
    stream_config.video_mode                 = cli_options.video_mode;
    stream_config.video_codec_mask           = cli_options.video_codec_mask;
    stream_config.video_decoder_mode          = cli_options.video_decoder_mode;
    stream_config.disable_vsync              = cli_options.disable_vsync;
    stream_config.disable_audio              = cli_options.disable_audio;

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

    if (!waydisplay::client_connect(state, cli_options.server_host.c_str(), cli_options.tcp_port, cli_options.client_udp_port,
                                    stream_config, cli_options.desired_width, cli_options.desired_height))
    {
        waydisplay::client_disconnect(state);
        SDL_Quit();
        return 1;
    }

    if (!waydisplay::client_start_network_worker(state))
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
