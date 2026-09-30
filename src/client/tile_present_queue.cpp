#include "tile_present_queue.hpp"

#include <algorithm>
#include <iterator>

namespace waydisplay {

ClientTilePresentPushResult ClientTilePresentQueue::push(ClientTileUpload&& upload) {
    if (!upload.valid() || upload.byte_size() > max_bytes_)
    {
        return ClientTilePresentPushResult::Rejected;
    }

    /* A newer completion for the exact same visible rectangle supersedes one
     * that has not reached the renderer yet. Remove the old item and append the
     * replacement at the tail so its position still reflects completion order
     * relative to differently-sized overlapping rectangles. */
    for (auto it = uploads_.rbegin(); it != uploads_.rend(); ++it)
    {
        if (it->rect.x == upload.rect.x && it->rect.y == upload.rect.y &&
            it->rect.w == upload.rect.w && it->rect.h == upload.rect.h &&
            it->ownership_epoch == upload.ownership_epoch)
        {
            if (upload.generation < it->generation)
            {
                return ClientTilePresentPushResult::Superseded;
            }

            const size_t old_size = it->byte_size();
            if (bytes_ - old_size > max_bytes_ - upload.byte_size())
            {
                return ClientTilePresentPushResult::Rejected;
            }

            auto erase_it = std::next(it).base();
            bytes_ -= old_size;
            uploads_.erase(erase_it);
            bytes_ += upload.byte_size();
            uploads_.push_back(std::move(upload));
            return ClientTilePresentPushResult::Queued;
        }
    }

    if (uploads_.size() >= max_items_ || bytes_ > max_bytes_ - upload.byte_size())
    {
        return ClientTilePresentPushResult::Rejected;
    }

    bytes_ += upload.byte_size();
    uploads_.push_back(std::move(upload));
    return ClientTilePresentPushResult::Queued;
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
