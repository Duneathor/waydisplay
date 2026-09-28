#pragma once

#include "stream_ownership.h"
#include "tile_present_queue.hpp"

#include <cstdint>

namespace waydisplay {

/* Tile packets carry the server's remote content epoch, while the renderer
 * fences in-flight uploads with a client-local ownership epoch. Keep the
 * conversion policy in one place so producer and consumer stay consistent. */
inline uint64_t client_tile_upload_epoch(
    const wd_client_content_ownership_snapshot& ownership) noexcept {
    return ownership.epoch;
}

inline bool client_tile_upload_matches_ownership(
    const ClientTileUpload& upload,
    const wd_client_content_ownership_snapshot& ownership) noexcept {
    return ownership.owner == WD_CLIENT_CONTENT_OWNER_TILES && upload.ownership_epoch == ownership.epoch;
}

} // namespace waydisplay
