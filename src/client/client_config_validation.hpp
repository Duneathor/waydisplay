#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstdint>

namespace waydisplay {

enum class ClientConfigValidationError : uint8_t {
    None,
    MissingSession,
    MissingConnectionIdentity,
    UnsupportedDisplayDimensions,
    UnsupportedFramebufferSize,
    UnsupportedTileGeometry,
    InconsistentTileGrid,
    UnsupportedPixelFormat,
    UnsupportedCompression,
    InvalidUdpPayloadTarget,
};

bool client_normalize_and_validate_server_config(wd_server_config_payload& config,
                                                 ClientConfigValidationError* out_error = nullptr);
const char* client_config_validation_error_name(ClientConfigValidationError error);

} // namespace waydisplay
