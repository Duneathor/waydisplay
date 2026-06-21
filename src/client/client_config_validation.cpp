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
    if (config.config_epoch == 0)
    {
        return fail(ClientConfigValidationError::MissingConfigurationEpoch, out_error);
    }

    if (config.width == 0 || config.height == 0 || config.width > WD_MAX_RENDER_WIDTH ||
        config.height > WD_MAX_RENDER_HEIGHT)
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
    if (config.compression_mode != WD_COMPRESSION_ZSTD || config.zstd_level == 0 || config.zstd_level > 22)
    {
        return fail(ClientConfigValidationError::UnsupportedCompression, out_error);
    }

    if ((config.capabilities & ~WD_SERVER_CAP_MASK) != 0 ||
        (config.video_codecs & ~WD_VIDEO_CODEC_MASK) != 0)
    {
        return fail(ClientConfigValidationError::InvalidCapabilities, out_error);
    }
    const bool video = (config.capabilities & WD_SERVER_CAP_VIDEO_STREAM) != 0;
    if ((video && (config.video_codecs == 0 || config.video_transport != WD_VIDEO_TRANSPORT_TCP)) ||
        (!video && (config.video_codecs != 0 || config.video_transport != 0)))
    {
        return fail(ClientConfigValidationError::InvalidCapabilities, out_error);
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
        case ClientConfigValidationError::MissingConfigurationEpoch:
            return "missing configuration epoch";
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
        case ClientConfigValidationError::InvalidCapabilities:
            return "invalid capabilities";
        case ClientConfigValidationError::InvalidUdpPayloadTarget:
            return "invalid UDP payload target";
    }
    return "unknown";
}


uint32_t client_classify_server_config_change(const wd_server_config_payload& current,
                                              const wd_server_config_payload& next) {
    uint32_t flags = ClientConfigChangeNone;
    if (current.config_epoch != next.config_epoch)
    {
        flags |= ClientConfigChangeEpoch;
    }
    if (current.width != next.width || current.height != next.height ||
        current.tile_width != next.tile_width || current.tile_height != next.tile_height ||
        current.tiles_x != next.tiles_x || current.tiles_y != next.tiles_y ||
        current.total_tiles != next.total_tiles)
    {
        flags |= ClientConfigChangeGeometry;
    }
    /* session/token describe the transport lifetime. Geometry and epoch
     * changes intentionally do not force UDP receiver recreation. */
    if (current.session_id != next.session_id ||
        current.connection_token != next.connection_token ||
        current.server_udp_port != next.server_udp_port ||
        current.udp_payload_target != next.udp_payload_target)
    {
        flags |= ClientConfigChangeTransport;
    }
    if (current.content_epoch != next.content_epoch)
    {
        flags |= ClientConfigChangeContent;
    }
    if (current.pixel_format != next.pixel_format || current.compression_mode != next.compression_mode ||
        current.zstd_level != next.zstd_level)
    {
        flags |= ClientConfigChangeFormat;
    }
    if (current.capabilities != next.capabilities || current.video_codecs != next.video_codecs ||
        current.video_transport != next.video_transport)
    {
        flags |= ClientConfigChangeVideo;
    }
    if (current.link_rtt_ms != next.link_rtt_ms ||
        current.summary_retransmit_grace_ms != next.summary_retransmit_grace_ms ||
        current.retransmit_rerequest_ms != next.retransmit_rerequest_ms ||
        current.retransmit_inflight_grace_ms != next.retransmit_inflight_grace_ms ||
        current.tile_reassembly_timeout_ms != next.tile_reassembly_timeout_ms ||
        current.active_summary_interval_ms != next.active_summary_interval_ms ||
        current.clean_summary_interval_ms != next.clean_summary_interval_ms)
    {
        flags |= ClientConfigChangeTimers;
    }
    return flags;
}

bool client_config_change_requires_stream_reset(uint32_t flags) {
    return (flags & (ClientConfigChangeGeometry | ClientConfigChangeTransport |
                     ClientConfigChangeContent | ClientConfigChangeFormat)) != 0;
}

} // namespace waydisplay
