#include "video_present_queue.hpp"

#include <utility>

namespace waydisplay {

ClientVideoFrameBuffer ClientVideoPresentQueue::take_decode_buffer(bool& dropped_newest) {
    dropped_newest = false;
    if (frames_.size() >= capacity_)
    {
        ClientVideoFrameBuffer buffer = std::move(frames_.back().buffer);
        frames_.pop_back();
        dropped_newest = true;
        return buffer;
    }

    ClientVideoFrameBuffer buffer = std::move(recycle_);
    recycle_.clear();
    return buffer;
}

bool ClientVideoPresentQueue::push_decoded(ClientVideoFrameBuffer&& buffer, uint32_t width,
                                           uint32_t height, uint64_t frame_id,
                                           uint64_t pts_usec, uint64_t epoch) {
    if (!buffer.valid() || buffer.width != width || buffer.height != height ||
        frames_.size() >= capacity_)
    {
        recycle(std::move(buffer));
        return false;
    }

    ClientQueuedVideoFrame frame{};
    frame.buffer = std::move(buffer);
    frame.width = width;
    frame.height = height;
    frame.frame_id = frame_id;
    frame.pts_usec = pts_usec;
    frame.epoch = epoch;
    frames_.push_back(std::move(frame));
    return true;
}

const ClientQueuedVideoFrame* ClientVideoPresentQueue::front() const {
    return frames_.empty() ? nullptr : &frames_.front();
}

ClientQueuedVideoFrame ClientVideoPresentQueue::pop_front() {
    if (frames_.empty())
    {
        return ClientQueuedVideoFrame{};
    }
    ClientQueuedVideoFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

void ClientVideoPresentQueue::recycle(ClientVideoFrameBuffer&& buffer) {
    recycle_ = std::move(buffer);
}

void ClientVideoPresentQueue::clear() {
    frames_.clear();
    recycle_.clear();
}

size_t ClientVideoPresentQueue::size() const {
    return frames_.size();
}

size_t ClientVideoPresentQueue::capacity() const {
    return capacity_;
}

bool ClientVideoPresentQueue::empty() const {
    return frames_.empty();
}

} // namespace waydisplay
