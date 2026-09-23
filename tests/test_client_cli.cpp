#include "client_cli.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                     \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

using waydisplay::ClientCliOptions;
using waydisplay::ClientCliParseResult;

static ClientCliParseResult parse(std::initializer_list<const char*> arguments, ClientCliOptions& options, std::string& error) {
    std::vector<const char*> argv(arguments);
    return waydisplay::client_cli_parse(static_cast<int>(argv.size()), argv.data(), options, &error);
}

static void test_defaults(void) {
    ClientCliOptions options;
    std::string      error;
    CHECK(parse({"client", "192.0.2.1"}, options, error) == ClientCliParseResult::Ok);
    CHECK(options.server_host == "192.0.2.1");
    CHECK(options.tcp_port == 5000);
    CHECK(options.client_udp_port == 6000);
    CHECK(options.requested_session_fps == WD_CLIENT_DEFAULT_SESSION_FPS);
    CHECK(options.link_cap_kib_per_second == 0);
    CHECK(options.video_mode == WD_VIDEO_MODE_AUTO);
    CHECK(options.video_codec_mask == WD_VIDEO_CODEC_H265);
    CHECK(options.video_decoder_mode == WD_CLIENT_VIDEO_DECODER_AUTO);
    CHECK(options.desired_width == 0 && options.desired_height == 0);
    CHECK(!options.disable_vsync && !options.disable_audio);
    CHECK(!options.verbose);
    CHECK(parse({"client", "192.0.2.1", "5500"}, options, error) == ClientCliParseResult::Ok);
    CHECK(options.tcp_port == 5500 && options.client_udp_port == WD_CLIENT_DEFAULT_UDP_PORT);
    CHECK(parse({"client", "192.0.2.1", "5500", "6500"}, options, error) == ClientCliParseResult::Ok);
    CHECK(options.tcp_port == 5500 && options.client_udp_port == 6500);
    CHECK(parse({"client", "192.0.2.1", "--video-codec", "av1", "-v"}, options, error) == ClientCliParseResult::Ok);
    CHECK(options.verbose && options.video_codec_mask == WD_VIDEO_CODEC_AV1);
    CHECK(options.tcp_port == WD_DEFAULT_TCP_PORT && options.client_udp_port == WD_CLIENT_DEFAULT_UDP_PORT);
    CHECK(parse({"client", "--verbose", "192.0.2.1", "5500", "6500"}, options, error) == ClientCliParseResult::Ok);
    CHECK(options.verbose && options.tcp_port == 5500 && options.client_udp_port == 6500);
}

static void test_av1_option(void) {
    ClientCliOptions options;
    std::string error;
    CHECK(parse({"client", "192.0.2.1", "5000", "6000", "--video-codec", "av1"}, options, error) == ClientCliParseResult::Ok);
    CHECK(options.video_codec_mask == WD_VIDEO_CODEC_AV1);
}

static void test_retained_options(void) {
    ClientCliOptions options;
    std::string      error;
    CHECK(parse({"client", "198.51.100.8", "5500", "6500", "--session-fps", "75", "--display-size", "1920x1080", "--link-cap-kib-per-sec", "8192", "--no-vsync",
                 "--no-audio", "--video-mode", "force", "--video-codec", "auto", "--video-decoder", "software"},
                options, error) == ClientCliParseResult::Ok);
    CHECK(options.requested_session_fps == 75);
    CHECK(options.desired_width == 1920 && options.desired_height == 1080);
    CHECK(options.link_cap_kib_per_second == 8192);
    CHECK(options.disable_vsync && options.disable_audio);
    CHECK(options.video_mode == WD_VIDEO_MODE_FORCE);
    CHECK(options.video_codec_mask == (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265));
    CHECK(options.video_decoder_mode == WD_CLIENT_VIDEO_DECODER_SOFTWARE);
}

static void test_removed_options_are_rejected(void) {
    const char* removed[][2] = {
        {"--limited-rate-kib", "4096"},
        {"--wan", nullptr},
        {"--mode", "limited"},
        {"--video-bitrate-kib", "8192"},
        {"--video-min-dirty-percent", "60"},
        {"--video-enter-seconds", "3"},
        {"--video-exit-dirty-percent", "30"},
        {"--video-exit-seconds", "30"},
        {"--video-hwdecode", "off"},
        {"--fps", "60"},
        {"--size", "1280x720"},
        {"--rate-kib", "4096"},
        {"--video", "force"},
        {"--video-decode", "software"},
    };

    for (const auto& option : removed)
    {
        ClientCliOptions options;
        std::string      error;
        if (option[1])
        {
            CHECK(parse({"client", "127.0.0.1", "5000", "6000", option[0], option[1]}, options, error) == ClientCliParseResult::Error);
        }
        else
        {
            CHECK(parse({"client", "127.0.0.1", "5000", "6000", option[0]}, options, error) == ClientCliParseResult::Error);
        }
    }
}

static void test_decode_modes(void) {
    struct ModeCase { const char* name; uint8_t mode; };
    const ModeCase modes[] = {
        {"off", WD_CLIENT_VIDEO_DECODER_OFF},
        {"auto", WD_CLIENT_VIDEO_DECODER_AUTO},
        {"software", WD_CLIENT_VIDEO_DECODER_SOFTWARE},
        {"vaapi", WD_CLIENT_VIDEO_DECODER_VAAPI},
    };
    for (const auto& mode : modes)
    {
        ClientCliOptions options;
        std::string error;
        CHECK(parse({"client", "127.0.0.1", "5000", "6000", "--video-decoder", mode.name}, options, error) ==
              ClientCliParseResult::Ok);
        CHECK(options.video_decoder_mode == mode.mode);
    }
    ClientCliOptions options;
    std::string error;
    CHECK(parse({"client", "127.0.0.1", "5000", "6000", "--video-decoder", "hardware"}, options, error) ==
          ClientCliParseResult::Error);
    CHECK(error.find("--video-decoder") != std::string::npos);
    CHECK(parse({"client", "127.0.0.1", "5000", "6000", "--video-decoder"}, options, error) ==
          ClientCliParseResult::Error);
}

static void test_invalid_values(void) {
    const char* invalid_sizes[] = {"", "0x1", "1x0", "1X1", "1x1junk", "4097x1", "1x2161"};
    for (const char* value : invalid_sizes)
    {
        ClientCliOptions options;
        std::string      error;
        CHECK(parse({"client", "127.0.0.1", "5000", "6000", "--display-size", value}, options, error) == ClientCliParseResult::Error);
    }

    ClientCliOptions options;
    std::string      error;
    CHECK(parse({"client", "127.0.0.1", "0", "6000"}, options, error) == ClientCliParseResult::Error);
    const std::string excessive_fps = std::to_string(static_cast<uint64_t>(WD_MAX_SESSION_FPS) + 1u);
    CHECK(parse({"client", "127.0.0.1", "5000", "6000", "--session-fps", excessive_fps.c_str()}, options, error) == ClientCliParseResult::Error);
    CHECK(parse({"client"}, options, error) == ClientCliParseResult::Error);
    CHECK(parse({"client", "-v"}, options, error) == ClientCliParseResult::Error);
    CHECK(parse({"client", "127.0.0.1", "5000", "6000", "7000"}, options, error) == ClientCliParseResult::Error);
    CHECK(parse({"client", "127.0.0.1", "bad"}, options, error) == ClientCliParseResult::Error);
    CHECK(parse({"client", "127.0.0.1", "--nonsense"}, options, error) == ClientCliParseResult::Error);
    CHECK(parse({"client", "127.0.0.1", "5000", "6000", "--link-cap-kib-per-sec", "+1"}, options, error) == ClientCliParseResult::Error);
}

static void test_help(void) {
    ClientCliOptions options;
    std::string      error;
    CHECK(parse({"client", "--help"}, options, error) == ClientCliParseResult::Help);
    CHECK(parse({"client", "127.0.0.1", "5000", "6000", "-h"}, options, error) == ClientCliParseResult::Help);
}

int main() {
    test_defaults();
    test_av1_option();
    test_retained_options();
    test_decode_modes();
    test_removed_options_are_rejected();
    test_invalid_values();
    test_help();
    return 0;
}
