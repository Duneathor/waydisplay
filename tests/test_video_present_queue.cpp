#include "video_present_queue.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

ClientVideoFrameBuffer make_frame(uint32_t width, uint32_t height, uint8_t fill) {
    ClientVideoFrameBuffer frame{};
    frame.format = ClientVideoPixelFormat::IYUV;
    frame.width = width;
    frame.height = height;
    frame.y_pitch = width;
    frame.uv_pitch = (width + 1u) / 2u;
    frame.u_offset = static_cast<size_t>(frame.y_pitch) * height;
    const uint32_t chroma_height = (height + 1u) / 2u;
    frame.v_offset = frame.u_offset + static_cast<size_t>(frame.uv_pitch) * chroma_height;
    frame.bytes.assign(frame.v_offset + static_cast<size_t>(frame.uv_pitch) * chroma_height, fill);
    require(frame.valid(), "test frame should be valid");
    return frame;
}

void test_zero_capacity_normalizes_to_one() {
    ClientVideoPresentQueue queue(0);
    require(queue.capacity() == 1, "zero capacity should normalize to one frame");
    require(queue.empty() && queue.size() == 0, "new queue should be empty");
    require(queue.front() == nullptr, "empty queue should have no front frame");

    ClientQueuedVideoFrame empty = queue.pop_front();
    require(!empty.buffer.valid() && empty.frame_id == 0, "empty pop should return an empty frame");
}

void test_rejects_invalid_and_mismatched_frames() {
    ClientVideoPresentQueue queue(2);
    ClientVideoFrameBuffer invalid{};
    require(!queue.push_decoded(std::move(invalid), 4, 4, 1, 10, 1),
            "invalid decoded buffers must be rejected");

    ClientVideoFrameBuffer mismatched = make_frame(4, 4, 7);
    require(!queue.push_decoded(std::move(mismatched), 8, 4, 2, 20, 1),
            "metadata dimensions must match the decoded buffer");

    bool dropped_newest = true;
    ClientVideoFrameBuffer recycled = queue.take_decode_buffer(dropped_newest);
    require(!dropped_newest, "taking a recycle buffer from a non-full queue is not an overflow drop");
    require(recycled.valid() && recycled.width == 4 && recycled.height == 4,
            "a rejected valid buffer should remain available for decoder reuse");
    require(queue.empty(), "rejected frames must not enter the presentation queue");
}

void test_full_queue_discards_tail_but_preserves_head() {
    ClientVideoPresentQueue queue(2);
    require(queue.push_decoded(make_frame(4, 4, 1), 4, 4, 10, 100, 3), "queue first frame");
    require(queue.push_decoded(make_frame(4, 4, 2), 4, 4, 11, 200, 3), "queue second frame");

    bool dropped_newest = false;
    ClientVideoFrameBuffer reuse = queue.take_decode_buffer(dropped_newest);
    require(dropped_newest, "full queue should discard the newest waiting frame");
    require(queue.size() == 1 && queue.front() && queue.front()->frame_id == 10,
            "overflow must preserve the oldest frame waiting for presentation");
    require(reuse.valid() && reuse.bytes.front() == 2, "discarded tail storage should be reused");

    std::fill(reuse.bytes.begin(), reuse.bytes.end(), 3);
    require(queue.push_decoded(std::move(reuse), 4, 4, 12, 300, 4),
            "new decode should replace the discarded tail");
    require(queue.size() == 2, "queue should return to capacity");

    ClientQueuedVideoFrame first = queue.pop_front();
    require(first.frame_id == 10 && first.pts_usec == 100 && first.epoch == 3,
            "head metadata should survive overflow");
    ClientQueuedVideoFrame second = queue.pop_front();
    require(second.frame_id == 12 && second.pts_usec == 300 && second.epoch == 4,
            "replacement frame should follow the held head");
}

void test_recycle_and_clear_release_all_state() {
    ClientVideoPresentQueue queue(3);
    ClientVideoFrameBuffer recycled = make_frame(6, 2, 9);
    queue.recycle(std::move(recycled));

    bool dropped_newest = true;
    ClientVideoFrameBuffer decode = queue.take_decode_buffer(dropped_newest);
    require(!dropped_newest && decode.valid() && decode.width == 6,
            "explicit recycle buffer should be returned exactly once");
    ClientVideoFrameBuffer empty = queue.take_decode_buffer(dropped_newest);
    require(!dropped_newest && !empty.valid(), "recycle slot should be empty after it is taken");

    require(queue.push_decoded(make_frame(4, 4, 1), 4, 4, 1, 1, 1), "queue frame before clear");
    queue.recycle(make_frame(2, 2, 2));
    queue.clear();
    require(queue.empty() && queue.front() == nullptr, "clear should discard all queued frames");
    ClientVideoFrameBuffer after_clear = queue.take_decode_buffer(dropped_newest);
    require(!after_clear.valid(), "clear should also discard the recycle buffer");
}

} // namespace

int main() {
    test_zero_capacity_normalizes_to_one();
    test_rejects_invalid_and_mismatched_frames();
    test_full_queue_discards_tail_but_preserves_head();
    test_recycle_and_clear_release_all_state();
    return 0;
}
