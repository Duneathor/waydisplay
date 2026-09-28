#include "tile_present_queue.hpp"

#include <algorithm>

namespace waydisplay {

bool ClientTilePresentQueue::push(ClientTileUpload&& upload) {
    if (!upload.valid() || upload.byte_size() > max_bytes_)
    {
        return false;
    }

    /* A newer completion for the exact same visible rectangle supersedes one
     * that has not reached the renderer yet. This keeps interactive bursts
     * bounded without discarding the newest visual state. */
    for (auto it = uploads_.rbegin(); it != uploads_.rend(); ++it)
    {
        if (it->rect.x == upload.rect.x && it->rect.y == upload.rect.y &&
            it->rect.w == upload.rect.w && it->rect.h == upload.rect.h &&
            it->ownership_epoch == upload.ownership_epoch)
        {
            if (upload.generation < it->generation)
            {
                return true;
            }
            const size_t old_size = it->byte_size();
            if (bytes_ - old_size > max_bytes_ - upload.byte_size())
            {
                return false;
            }
            bytes_ -= old_size;
            *it = std::move(upload);
            bytes_ += it->byte_size();
            return true;
        }
    }

    if (uploads_.size() >= max_items_ || bytes_ > max_bytes_ - upload.byte_size())
    {
        return false;
    }

    bytes_ += upload.byte_size();
    uploads_.push_back(std::move(upload));
    return true;
}

void ClientTilePresentQueue::drain(std::vector<ClientTileUpload>& out) {
    out.clear();
    out.reserve(uploads_.size());
    while (!uploads_.empty())
    {
        out.push_back(std::move(uploads_.front()));
        uploads_.pop_front();
    }
    bytes_ = 0;
}

void ClientTilePresentQueue::clear() {
    uploads_.clear();
    bytes_ = 0;
}

} // namespace waydisplay
