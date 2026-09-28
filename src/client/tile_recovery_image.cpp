#include "tile_recovery_image.hpp"

#include <cstring>
#include <limits>

namespace waydisplay {

bool client_apply_tile_upload_to_recovery(std::vector<uint32_t>& framebuffer,
                                          uint32_t frame_width,
                                          uint32_t frame_height,
                                          const ClientTileUpload& upload) {
    if (!upload.valid() || frame_width == 0 || frame_height == 0)
    {
        return false;
    }

    const uint32_t right = static_cast<uint32_t>(upload.rect.x) + upload.rect.w;
    const uint32_t bottom = static_cast<uint32_t>(upload.rect.y) + upload.rect.h;
    if (right > frame_width || bottom > frame_height)
    {
        return false;
    }
    if (frame_width > std::numeric_limits<size_t>::max() / frame_height ||
        framebuffer.size() < static_cast<size_t>(frame_width) * frame_height)
    {
        return false;
    }

    for (uint32_t row = 0; row < upload.rect.h; ++row)
    {
        const uint8_t* src = upload.pixels.data() + static_cast<size_t>(row) * upload.source_pitch;
        uint32_t* dst = framebuffer.data() +
                        static_cast<size_t>(upload.rect.y + row) * frame_width + upload.rect.x;
        std::memcpy(dst, src, static_cast<size_t>(upload.rect.w) * sizeof(uint32_t));
    }
    return true;
}

} // namespace waydisplay
