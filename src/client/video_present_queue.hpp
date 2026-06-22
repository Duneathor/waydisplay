#pragma once

#include "video_decoder.hpp"

#include "waydisplay/wd_config.h"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace waydisplay {


struct ClientQueuedVideoFrame {
    ClientVideoFrameBuffer buffer{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_id = 0;
    uint64_t pts_usec = 0;
    uint64_t epoch = 0;
};

class ClientVideoPresentQueue {
  public:
    explicit ClientVideoPresentQueue(size_t capacity = WD_CLIENT_VIDEO_PRESENT_QUEUE_CAPACITY)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    ClientVideoFrameBuffer take_decode_buffer(bool& dropped_newest);
    bool push_decoded(ClientVideoFrameBuffer&& buffer, uint32_t width, uint32_t height,
                      uint64_t frame_id, uint64_t pts_usec, uint64_t epoch);

    const ClientQueuedVideoFrame* front() const;
    ClientQueuedVideoFrame pop_front();
    void recycle(ClientVideoFrameBuffer&& buffer);
    void clear();

    size_t size() const;
    size_t capacity() const;
    bool empty() const;

  private:
    size_t capacity_ = WD_CLIENT_VIDEO_PRESENT_QUEUE_CAPACITY;
    std::deque<ClientQueuedVideoFrame> frames_{};
    ClientVideoFrameBuffer recycle_{};
};

} // namespace waydisplay
