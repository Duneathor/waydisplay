#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstdint>

namespace waydisplay {

enum class ClientConfigValidationError : uint8_t {
    None,
    MissingSession,
    MissingConnectionIdentity,
    MissingConfigurationEpoch,
    UnsupportedDisplayDimensions,
    UnsupportedFramebufferSize,
    UnsupportedTileGeometry,
    InconsistentTileGrid,
    UnsupportedPixelFormat,
    UnsupportedCompression,
    InvalidCapabilities,
    InvalidUdpPayloadTarget,
};

bool client_normalize_and_validate_server_config(wd_server_config_payload& config,
                                                 ClientConfigValidationError* out_error = nullptr);
const char* client_config_validation_error_name(ClientConfigValidationError error);

enum ClientConfigChangeFlag : uint32_t {
    ClientConfigChangeNone      = 0,
    ClientConfigChangeGeometry  = 1u << 0,
    ClientConfigChangeTransport = 1u << 1,
    ClientConfigChangeContent   = 1u << 2,
    ClientConfigChangeFormat    = 1u << 3,
    ClientConfigChangeVideo     = 1u << 4,
    ClientConfigChangeTimers    = 1u << 5,
    ClientConfigChangeEpoch     = 1u << 6,
};

uint32_t client_classify_server_config_change(const wd_server_config_payload& current,
                                              const wd_server_config_payload& next);
bool client_config_change_requires_stream_reset(uint32_t flags);


} // namespace waydisplay
