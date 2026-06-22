#include "client_cli.hpp"
#include "client_net.hpp"
#include "sdl_viewer.hpp"
#include "wd_client.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s <server_ipv4> <tcp_port> <client_udp_port> [options]\n\n"
                 "Options:\n"
                 "  --fps <N>                     Requested remote capture FPS cap, default %u\n"
                 "  --size <WxH>                  Request remote display size\n"
                 "  --rate-kib <N>                Cap adaptive UDP tile budget in KiB/s\n"
                 "  --no-vsync                    Disable SDL present-vsync\n"
                 "  --no-audio                    Disable audio negotiation and playback\n"
                 "  --video <auto|off|force>      Select coarse video-mode policy, default auto\n"
                 "  --video-codec <auto|h264|h265> Select acceptable video codecs, default h265\n"
                 "  --video-hwdecode <off|auto|vaapi> Select hardware decoding, default auto\n"
                 "  --help, -h                    Show this help\n\n"
                 "Detailed stream thresholds and codec policy are configured in wd_config.h.\n\n"
                 "Examples:\n"
                 "  %s 127.0.0.1 5000 6000\n"
                 "  %s 192.168.1.50 5000 6000 --fps 60 --rate-kib 4096\n",
                 argv0, WD_CLIENT_DEFAULT_TARGET_FPS, argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    waydisplay::ClientCliOptions cli_options;
    std::string error_message;
    const std::vector<const char*> arguments(argv, argv + argc);
    const auto parse_result = waydisplay::client_cli_parse(
        argc, arguments.data(), cli_options, &error_message);
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

    waydisplay::ClientStreamConfig stream_config;
    stream_config.target_fps = cli_options.target_fps;
    stream_config.limited_udp_kib_per_second = cli_options.limited_udp_kib_per_second;
    stream_config.video_mode = cli_options.video_mode;
    stream_config.video_codec_mask = cli_options.video_codec_mask;
    stream_config.video_hwdecode_mode = cli_options.video_hwdecode_mode;
    stream_config.disable_vsync = cli_options.disable_vsync;
    stream_config.disable_audio = cli_options.disable_audio;

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

    if (!waydisplay::client_connect(state, cli_options.server_host.c_str(),
                                    cli_options.tcp_port, cli_options.client_udp_port,
                                    stream_config, cli_options.desired_width,
                                    cli_options.desired_height))
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
