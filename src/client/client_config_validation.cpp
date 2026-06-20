#include "client_config_validation.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_tile.h"

#include <cstdint>

namespace waydisplay {
namespace {

bool fail(ClientConfigValidationError error, ClientConfigValidationError* out_error) {
    if (out_error)
    {
        *out_error = error;
    }
    return false;
}

} // namespace

bool client_normalize_and_validate_server_config(wd_server_config_payload& config,
                                                 ClientConfigValidationError* out_error) {
    if (out_error)
    {
        *out_error = ClientConfigValidationError::None;
    }

    if (config.session_id == 0)
    {
        return fail(ClientConfigValidationError::MissingSession, out_error);
    }
    if (config.connection_token == 0 || config.content_epoch == 0 || config.server_udp_port == 0)
    {
        return fail(ClientConfigValidationError::MissingConnectionIdentity, out_error);
    }

    if (config.width == 0 || config.height == 0 || config.width > WD_CLIENT_MAX_DIMENSION ||
        config.height > WD_CLIENT_MAX_DIMENSION)
    {
        return fail(ClientConfigValidationError::UnsupportedDisplayDimensions, out_error);
    }

    const uint64_t framebuffer_pixels = static_cast<uint64_t>(config.width) * config.height;
    const uint64_t framebuffer_bytes = framebuffer_pixels * WD_BYTES_PER_PIXEL;
    if (framebuffer_pixels > UINT32_MAX || framebuffer_bytes > WD_CLIENT_MAX_FRAMEBUFFER_BYTES ||
        framebuffer_bytes > UINT32_MAX)
    {
        return fail(ClientConfigValidationError::UnsupportedFramebufferSize, out_error);
    }

    uint8_t tile_size = 0;
    if (!wd_tile_size_code_for_dimensions(config.tile_width, config.tile_height, &tile_size))
    {
        return fail(ClientConfigValidationError::UnsupportedTileGeometry, out_error);
    }
    (void)tile_size;

    const uint32_t expected_tiles_x = wd_tiles_for_width_with_tile(config.width, config.tile_width);
    const uint32_t expected_tiles_y = wd_tiles_for_height_with_tile(config.height, config.tile_height);
    const uint64_t expected_total = static_cast<uint64_t>(expected_tiles_x) * expected_tiles_y;
    if (expected_tiles_x == 0 || expected_tiles_y == 0 || expected_tiles_x > UINT16_MAX ||
        expected_tiles_y > UINT16_MAX || expected_total == 0 || expected_total > UINT16_MAX ||
        config.tiles_x != expected_tiles_x || config.tiles_y != expected_tiles_y ||
        config.total_tiles != expected_total)
    {
        return fail(ClientConfigValidationError::InconsistentTileGrid, out_error);
    }

    if (config.pixel_format != WD_PIXEL_FORMAT_XRGB8888)
    {
        return fail(ClientConfigValidationError::UnsupportedPixelFormat, out_error);
    }
    if (config.compression_mode != WD_COMPRESSION_ZSTD)
    {
        return fail(ClientConfigValidationError::UnsupportedCompression, out_error);
    }

    if (config.udp_payload_target == 0)
    {
        config.udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }
    if (config.udp_payload_target < WD_MIN_PROBED_UDP_PAYLOAD ||
        config.udp_payload_target > WD_UDP_TILE_PAYLOAD_MAX)
    {
        return fail(ClientConfigValidationError::InvalidUdpPayloadTarget, out_error);
    }

    return true;
}

const char* client_config_validation_error_name(ClientConfigValidationError error) {
    switch (error)
    {
        case ClientConfigValidationError::None:
            return "none";
        case ClientConfigValidationError::MissingSession:
            return "missing session";
        case ClientConfigValidationError::MissingConnectionIdentity:
            return "missing connection identity";
        case ClientConfigValidationError::UnsupportedDisplayDimensions:
            return "unsupported display dimensions";
        case ClientConfigValidationError::UnsupportedFramebufferSize:
            return "unsupported framebuffer size";
        case ClientConfigValidationError::UnsupportedTileGeometry:
            return "unsupported tile geometry";
        case ClientConfigValidationError::InconsistentTileGrid:
            return "inconsistent tile grid";
        case ClientConfigValidationError::UnsupportedPixelFormat:
            return "unsupported pixel format";
        case ClientConfigValidationError::UnsupportedCompression:
            return "unsupported compression";
        case ClientConfigValidationError::InvalidUdpPayloadTarget:
            return "invalid UDP payload target";
    }
    return "unknown";
}

} // namespace waydisplay
