#pragma once

#include "render_planning.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace waydisplay {

struct ClientTileUpload {
    ClientDirtyRect      rect{};
    uint64_t             ownership_epoch = 0;
    uint64_t             generation    = 0;
    uint32_t             source_pitch  = 0;
    std::vector<uint8_t> pixels{};

    size_t byte_size() const {
        return pixels.size();
    }

    bool valid() const {
        return rect.w != 0 && rect.h != 0 && source_pitch >= static_cast<uint32_t>(rect.w) * 4u &&
               pixels.size() >= static_cast<size_t>(source_pitch) * rect.h;
    }
};

enum class ClientTilePresentPushResult : uint8_t {
    Queued,
    Superseded,
    Rejected,
};

class ClientTilePresentQueue {
  public:
    explicit ClientTilePresentQueue(size_t max_items = 1024,
                                    size_t max_bytes = 32u * 1024u * 1024u)
        : max_items_(max_items == 0 ? 1 : max_items),
          max_bytes_(max_bytes == 0 ? 1 : max_bytes) {
    }

    ClientTilePresentPushResult push(ClientTileUpload&& upload);
    void drain(std::vector<ClientTileUpload>& out);
    void clear();

    size_t size() const { return uploads_.size(); }
    size_t bytes() const { return bytes_; }
    bool empty() const { return uploads_.empty(); }

  private:
    size_t max_items_;
    size_t max_bytes_;
    size_t bytes_ = 0;
    std::deque<ClientTileUpload> uploads_{};
};

} // namespace waydisplay
