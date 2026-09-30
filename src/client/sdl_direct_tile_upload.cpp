#include "sdl_direct_tile_upload.hpp"

#include "client_state.hpp"
#include "tile_recovery_image.hpp"
#include "tile_upload_epoch.hpp"
#include "waydisplay/wd_time.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace waydisplay {
namespace {

void record_atomic_max_local(std::atomic<uint64_t>& value, uint64_t sample) {
    uint64_t current = value.load(std::memory_order_relaxed);
    while (sample > current && !value.compare_exchange_weak(current, sample, std::memory_order_relaxed,
                                                             std::memory_order_relaxed))
    {
    }
}

} // namespace

bool upload_completed_tiles_direct(ClientState& state, SDL_Texture* texture,
                                   std::vector<ClientTileUpload>& uploads,
                                   std::vector<ClientDirtyRect>& uploaded_rects) {
    uploaded_rects.clear();
    if (uploads.empty())
    {
        return true;
    }
    if (!texture)
    {
        return false;
    }

    uint64_t direct_present_count = 0;
    uint64_t lock_wait_samples    = 0;
    uint64_t lock_wait_sum_ns     = 0;
    uint64_t lock_wait_max_ns     = 0;
    const auto publish_batch = [&]() {
        if (direct_present_count != 0)
        {
            state.stats.tile_present_direct.fetch_add(direct_present_count, std::memory_order_relaxed);
        }
        if (lock_wait_samples != 0)
        {
            state.stats.lock_wait_samples.fetch_add(lock_wait_samples, std::memory_order_relaxed);
            state.stats.lock_wait_sum_ns.fetch_add(lock_wait_sum_ns, std::memory_order_relaxed);
            record_atomic_max_local(state.stats.lock_wait_max_ns, lock_wait_max_ns);
        }
    };

    const auto ownership = wd_client_stream_ownership_snapshot(&state.stream_ownership);
    for (ClientTileUpload& upload : uploads)
    {
        if (!upload.valid() || !client_tile_upload_matches_ownership(upload, ownership))
        {
            continue;
        }

        SDL_Rect rect{
            static_cast<int>(upload.rect.x), static_cast<int>(upload.rect.y),
            static_cast<int>(upload.rect.w), static_cast<int>(upload.rect.h)};
        if (!SDL_UpdateTexture(texture, &rect, upload.pixels.data(), static_cast<int>(upload.source_pitch)))
        {
            publish_batch();
            return false;
        }
        ++direct_present_count;

        {
            std::unique_lock<std::mutex> framebuffer_lock(state.framebuffer_mutex, std::defer_lock);
            if (!framebuffer_lock.try_lock())
            {
                const uint64_t lock_started_ns = wd_now_ns();
                framebuffer_lock.lock();
                const uint64_t wait_ns = wd_now_ns() - lock_started_ns;
                ++lock_wait_samples;
                lock_wait_sum_ns += wait_ns;
                lock_wait_max_ns = std::max(lock_wait_max_ns, wait_ns);
            }
            if (!client_apply_tile_upload_to_recovery(state.framebuffer, state.config.width,
                                                      state.config.height, upload))
            {
                publish_batch();
                return false;
            }
        }
        uploaded_rects.push_back(upload.rect);
    }
    publish_batch();
    return true;
}

void recycle_direct_tile_upload_buffers(ClientState& state,
                                        std::vector<ClientTileUpload>& uploads) {
    std::lock_guard<std::mutex> recycle_lock(state.tile_present_recycle_mutex);
    for (ClientTileUpload& upload : uploads)
    {
        if (!upload.pixels.empty())
        {
            state.tile_present_recycled_buffers.push_back(std::move(upload.pixels));
        }
    }
}

} // namespace waydisplay
