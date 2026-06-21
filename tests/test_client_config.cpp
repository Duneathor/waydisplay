#include "client_config_validation.hpp"

#include "waydisplay/wd_config.h"

#include <cstdlib>
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

wd_server_config_payload valid_config() {
    wd_server_config_payload config{};
    config.session_id = 1;
    config.connection_token = 0x1234;
    config.content_epoch = 1;
    config.config_epoch = 1;
    config.server_udp_port = 5001;
    config.width = 1920;
    config.height = 1080;
    config.tile_width = 16;
    config.tile_height = 16;
    config.tiles_x = 120;
    config.tiles_y = 68;
    config.total_tiles = 8160;
    config.pixel_format = WD_PIXEL_FORMAT_XRGB8888;
    config.compression_mode = WD_COMPRESSION_ZSTD;
    config.zstd_level = 1;
    config.udp_payload_target = 1200;
    return config;
}

void test_accepts_and_normalizes_valid_config() {
    wd_server_config_payload config = valid_config();
    config.udp_payload_target = 0;
    ClientConfigValidationError error = ClientConfigValidationError::MissingSession;
    require(client_normalize_and_validate_server_config(config, &error), "valid config should be accepted");
    require(error == ClientConfigValidationError::None, "valid config error should be none");
    require(config.udp_payload_target == WD_UDP_PAYLOAD_TARGET, "zero UDP target should use default");
}

void test_rejects_missing_connection_identity() {
    wd_server_config_payload config = valid_config();
    config.connection_token = 0;
    ClientConfigValidationError error{};
    require(!client_normalize_and_validate_server_config(config, &error), "zero connection token should fail");
    require(error == ClientConfigValidationError::MissingConnectionIdentity, "connection identity error code");

    config = valid_config();
    config.content_epoch = 0;
    require(!client_normalize_and_validate_server_config(config, &error), "zero content epoch should fail");
    require(error == ClientConfigValidationError::MissingConnectionIdentity, "content epoch identity error code");

    config = valid_config();
    config.server_udp_port = 0;
    require(!client_normalize_and_validate_server_config(config, &error), "zero server UDP port should fail");
    require(error == ClientConfigValidationError::MissingConnectionIdentity, "UDP endpoint identity error code");
}

void test_rejects_unsupported_tile_geometry() {
    wd_server_config_payload config = valid_config();
    config.tile_width = 48;
    config.tile_height = 48;
    ClientConfigValidationError error{};
    require(!client_normalize_and_validate_server_config(config, &error), "unsupported tile geometry should fail");
    require(error == ClientConfigValidationError::UnsupportedTileGeometry, "tile geometry error code");
}

void test_rejects_inconsistent_grid() {
    wd_server_config_payload config = valid_config();
    config.tiles_x--;
    ClientConfigValidationError error{};
    require(!client_normalize_and_validate_server_config(config, &error), "inconsistent grid should fail");
    require(error == ClientConfigValidationError::InconsistentTileGrid, "grid error code");

    config = valid_config();
    config.total_tiles--;
    require(!client_normalize_and_validate_server_config(config, &error), "inconsistent total should fail");
}

void test_rejects_oversized_framebuffer() {
    wd_server_config_payload config = valid_config();
    config.width = WD_MAX_RENDER_WIDTH + 1u;
    ClientConfigValidationError error{};
    require(!client_normalize_and_validate_server_config(config, &error),
            "render width above the protocol limit should fail");
    require(error == ClientConfigValidationError::UnsupportedDisplayDimensions,
            "oversized geometry should report the dimension limit");

    config = valid_config();
    config.height = WD_MAX_RENDER_HEIGHT + 1u;
    require(!client_normalize_and_validate_server_config(config, &error),
            "render height above the protocol limit should fail");
}


void test_rejects_invalid_udp_payload_target() {
    wd_server_config_payload config = valid_config();
    config.udp_payload_target = WD_MIN_PROBED_UDP_PAYLOAD - 1;
    ClientConfigValidationError error{};
    require(!client_normalize_and_validate_server_config(config, &error), "small UDP target should fail");
    require(error == ClientConfigValidationError::InvalidUdpPayloadTarget, "UDP target error code");

    config = valid_config();
    config.udp_payload_target = static_cast<uint16_t>(WD_UDP_TILE_PAYLOAD_MAX + 1u);
    require(!client_normalize_and_validate_server_config(config, &error), "large UDP target should fail");
}

void test_classifies_transport_changes_independently_of_geometry() {
    wd_server_config_payload current = valid_config();
    wd_server_config_payload next = current;
    next.server_udp_port++;
    uint32_t flags = client_classify_server_config_change(current, next);
    require((flags & ClientConfigChangeTransport) != 0, "UDP endpoint change should be transport change");
    require((flags & ClientConfigChangeGeometry) == 0, "UDP endpoint change should not imply geometry change");
    require(client_config_change_requires_stream_reset(flags), "transport change must reset stream state");

    next = current;
    next.udp_payload_target -= 16;
    flags = client_classify_server_config_change(current, next);
    require((flags & ClientConfigChangeTransport) != 0, "UDP payload target change should recreate receiver");

    next = current;
    next.config_epoch++;
    next.content_epoch++;
    next.width = 1366;
    next.height = 720;
    next.tiles_x = 86;
    next.tiles_y = 45;
    next.total_tiles = 3870;
    flags = client_classify_server_config_change(current, next);
    require((flags & ClientConfigChangeGeometry) != 0, "resize should change geometry");
    require((flags & ClientConfigChangeContent) != 0, "resize should advance content epoch");
    require((flags & ClientConfigChangeEpoch) != 0, "resize should advance config epoch");
    require((flags & ClientConfigChangeTransport) == 0,
            "resize with stable session/token must retain UDP transport");

    next = current;
    next.link_rtt_ms++;
    flags = client_classify_server_config_change(current, next);
    require(flags == ClientConfigChangeTimers, "timer-only update should remain lightweight");
    require(!client_config_change_requires_stream_reset(flags), "timer-only update should not reset stream");
}


void test_rejects_missing_config_epoch_and_invalid_capabilities() {
    wd_server_config_payload config = valid_config();
    config.config_epoch = 0;
    ClientConfigValidationError error{};
    require(!client_normalize_and_validate_server_config(config, &error),
            "zero config epoch should fail");
    require(error == ClientConfigValidationError::MissingConfigurationEpoch,
            "zero config epoch should report the barrier identity failure");

    config = valid_config();
    config.capabilities = 0x80000000u;
    require(!client_normalize_and_validate_server_config(config, &error),
            "unknown server capabilities should fail");
    require(error == ClientConfigValidationError::InvalidCapabilities,
            "unknown server capabilities should report a capability failure");

    config = valid_config();
    config.capabilities = WD_SERVER_CAP_VIDEO_STREAM;
    config.video_codecs = 0;
    config.video_transport = WD_VIDEO_TRANSPORT_TCP;
    require(!client_normalize_and_validate_server_config(config, &error),
            "video capability without a codec should fail");
}

} // namespace

int main() {
    test_accepts_and_normalizes_valid_config();
    test_rejects_missing_connection_identity();
    test_rejects_unsupported_tile_geometry();
    test_rejects_inconsistent_grid();
    test_rejects_oversized_framebuffer();
    test_rejects_invalid_udp_payload_target();
    test_classifies_transport_changes_independently_of_geometry();
    test_rejects_missing_config_epoch_and_invalid_capabilities();
    return 0;
}
