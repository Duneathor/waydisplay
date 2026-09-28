#pragma once

#include "tile_present_queue.hpp"

#include <cstdint>
#include <vector>

namespace waydisplay {

/* Apply one already-validated direct tile upload to the CPU recovery image.
 * The recovery image is the source for later full SDL texture uploads, so it
 * must track every direct upload that is eligible for presentation. */
bool client_apply_tile_upload_to_recovery(std::vector<uint32_t>& framebuffer,
                                          uint32_t frame_width,
                                          uint32_t frame_height,
                                          const ClientTileUpload& upload);

} // namespace waydisplay
