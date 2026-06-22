#include "client_config_validation.hpp"

#include "waydisplay/wd_config.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

wd_server_config_payload base_config() {
    wd_server_config_payload config{};
    config.session_id = 1;
    config.connection_token = 2;
    config.content_epoch = 3;
    config.config_epoch = 4;
    config.server_udp_port = 5001;
    config.width = 1280;
    config.height = 720;
    config.tile_width = 16;
    config.tile_height = 16;
    config.tiles_x = 80;
    config.tiles_y = 45;
    config.total_tiles = 3600;
    config.pixel_format = WD_PIXEL_FORMAT_XRGB8888;
    config.compression_mode = WD_COMPRESSION_ZSTD;
    config.zstd_level = 1;
    config.udp_payload_target = 1400;
    return config;
}

void expect_error(wd_server_config_payload config, ClientConfigValidationError expected,
                  const char* message) {
    ClientConfigValidationError error = ClientConfigValidationError::None;
    require(!client_normalize_and_validate_server_config(config, &error), message);
    require(error == expected, "validation should report the expected error class");
}

void test_identity_format_and_compression_matrix() {
    wd_server_config_payload config = base_config();
    config.session_id = 0;
    expect_error(config, ClientConfigValidationError::MissingSession, "zero session should fail");

    config = base_config();
    config.width = 0;
    expect_error(config, ClientConfigValidationError::UnsupportedDisplayDimensions, "zero width should fail");
    config = base_config();
    config.height = 0;
    expect_error(config, ClientConfigValidationError::UnsupportedDisplayDimensions, "zero height should fail");

    config = base_config();
    config.pixel_format = 99;
    expect_error(config, ClientConfigValidationError::UnsupportedPixelFormat, "unknown pixel format should fail");

    config = base_config();
    config.compression_mode = 0;
    expect_error(config, ClientConfigValidationError::UnsupportedCompression, "missing compression should fail");
    config = base_config();
    config.zstd_level = 0;
    expect_error(config, ClientConfigValidationError::UnsupportedCompression, "zero zstd level should fail");
    config = base_config();
    config.zstd_level = 23;
    expect_error(config, ClientConfigValidationError::UnsupportedCompression, "zstd level above supported range should fail");

    config = base_config();
    require(client_normalize_and_validate_server_config(config, nullptr),
            "valid config should support callers that do not request an error code");
}

void test_video_capability_matrix() {
    wd_server_config_payload config = base_config();
    config.video_codecs = WD_VIDEO_CODEC_H265;
    expect_error(config, ClientConfigValidationError::InvalidCapabilities,
                 "video fields without capability should fail");

    config = base_config();
    config.capabilities = WD_SERVER_CAP_VIDEO_STREAM;
    config.video_codecs = WD_VIDEO_CODEC_H265;
    config.video_transport = WD_VIDEO_TRANSPORT_TCP;
    require(client_normalize_and_validate_server_config(config, nullptr),
            "canonical video capability should validate");

    config.video_transport = 0;
    expect_error(config, ClientConfigValidationError::InvalidCapabilities,
                 "video capability requires TCP transport");
    config = base_config();
    config.capabilities = WD_SERVER_CAP_VIDEO_STREAM;
    config.video_codecs = WD_VIDEO_CODEC_MASK + 1u;
    config.video_transport = WD_VIDEO_TRANSPORT_TCP;
    expect_error(config, ClientConfigValidationError::InvalidCapabilities,
                 "unknown video codec bits should fail");
}

wd_server_config_payload audio_config() {
    wd_server_config_payload config = base_config();
    config.capabilities = WD_SERVER_CAP_AUDIO_STREAM;
    config.media_clock_id = 1;
    config.audio_codec = WD_AUDIO_CODEC_OPUS;
    config.audio_transport = WD_AUDIO_TRANSPORT_TCP;
    config.audio_sample_rate = WD_AUDIO_SAMPLE_RATE_DEFAULT;
    config.audio_channels = 2;
    config.audio_frame_samples = WD_AUDIO_FRAME_SAMPLES_DEFAULT;
    config.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;
    config.audio_bitrate = WD_AUDIO_BITRATE_DEFAULT;
    return config;
}

void test_audio_capability_matrix() {
    wd_server_config_payload config = audio_config();
    require(client_normalize_and_validate_server_config(config, nullptr),
            "canonical audio capability should validate");

    const auto invalid_cases = std::array<wd_server_config_payload, 9>{
        [&] { auto value = audio_config(); value.audio_codec = 0; return value; }(),
        [&] { auto value = audio_config(); value.audio_transport = 0; return value; }(),
        [&] { auto value = audio_config(); value.audio_sample_rate++; return value; }(),
        [&] { auto value = audio_config(); value.audio_channels = 0; return value; }(),
        [&] { auto value = audio_config(); value.audio_channels = WD_AUDIO_CHANNELS_MAX + 1u; return value; }(),
        [&] { auto value = audio_config(); value.audio_reserved = 1; return value; }(),
        [&] { auto value = audio_config(); value.audio_frame_samples = 1; return value; }(),
        [&] { auto value = audio_config(); value.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_MIN - 1u; return value; }(),
        [&] { auto value = audio_config(); value.audio_bitrate = 0; return value; }(),
    };
    for (const wd_server_config_payload& invalid : invalid_cases)
    {
        expect_error(invalid, ClientConfigValidationError::InvalidCapabilities,
                     "invalid audio capability field should fail");
    }

    config = audio_config();
    config.media_clock_id = 0;
    expect_error(config, ClientConfigValidationError::InvalidCapabilities,
                 "audio capability requires a media clock");

    config = base_config();
    config.audio_codec = WD_AUDIO_CODEC_OPUS;
    expect_error(config, ClientConfigValidationError::InvalidCapabilities,
                 "audio fields without capability should fail");
}

void test_error_names_are_total() {
    const std::array<ClientConfigValidationError, 12> errors{
        ClientConfigValidationError::None,
        ClientConfigValidationError::MissingSession,
        ClientConfigValidationError::MissingConnectionIdentity,
        ClientConfigValidationError::MissingConfigurationEpoch,
        ClientConfigValidationError::UnsupportedDisplayDimensions,
        ClientConfigValidationError::UnsupportedFramebufferSize,
        ClientConfigValidationError::UnsupportedTileGeometry,
        ClientConfigValidationError::InconsistentTileGrid,
        ClientConfigValidationError::UnsupportedPixelFormat,
        ClientConfigValidationError::UnsupportedCompression,
        ClientConfigValidationError::InvalidCapabilities,
        ClientConfigValidationError::InvalidUdpPayloadTarget,
    };
    for (ClientConfigValidationError error : errors)
    {
        const char* name = client_config_validation_error_name(error);
        require(name != nullptr && name[0] != '\0' && std::strcmp(name, "unknown") != 0,
                "every public validation error should have a stable name");
    }
    require(std::strcmp(client_config_validation_error_name(static_cast<ClientConfigValidationError>(255)),
                        "unknown") == 0,
            "unknown validation errors should have a fallback name");
}

void test_config_change_flag_matrix() {
    const wd_server_config_payload current = audio_config();
    wd_server_config_payload next = current;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeNone,
            "identical configs should have no changes");

    next.config_epoch++;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeEpoch,
            "config epoch should classify independently");
    next = current;
    next.width--;
    require((client_classify_server_config_change(current, next) & ClientConfigChangeGeometry) != 0,
            "geometry field changes should classify as geometry");
    next = current;
    next.session_id++;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeTransport,
            "session change should classify as transport");
    next = current;
    next.content_epoch++;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeContent,
            "content epoch should classify independently");
    next = current;
    next.zstd_level++;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeFormat,
            "compression change should classify as format");
    next = current;
    next.video_codecs = WD_VIDEO_CODEC_H264;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeVideo,
            "video negotiation change should classify independently");
    next = current;
    next.audio_bitrate++;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeAudio,
            "audio negotiation change should classify independently");
    next = current;
    next.link_rtt_ms++;
    require(client_classify_server_config_change(current, next) == ClientConfigChangeTimers,
            "link timer change should classify independently");

    require(!client_config_change_requires_stream_reset(ClientConfigChangeNone),
            "no changes should not reset streams");
    require(!client_config_change_requires_stream_reset(ClientConfigChangeAudio | ClientConfigChangeVideo |
                                                         ClientConfigChangeTimers | ClientConfigChangeEpoch),
            "media parameters and timers alone should not reset framebuffer transport");
    require(client_config_change_requires_stream_reset(ClientConfigChangeGeometry) &&
                client_config_change_requires_stream_reset(ClientConfigChangeTransport) &&
                client_config_change_requires_stream_reset(ClientConfigChangeContent) &&
                client_config_change_requires_stream_reset(ClientConfigChangeFormat),
            "geometry, transport, ownership, and format changes should reset stream state");
}

} // namespace

int main() {
    test_identity_format_and_compression_matrix();
    test_video_capability_matrix();
    test_audio_capability_matrix();
    test_error_names_are_total();
    test_config_change_flag_matrix();
    return 0;
}
