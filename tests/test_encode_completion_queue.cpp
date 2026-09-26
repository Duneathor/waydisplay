#include "wd_encode_completion_queue.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_fifo_order() {
    uint16_t storage[4]{};
    wd_encode_completion_queue queue{};
    wd_encode_completion_queue_init(&queue, storage, 4);

    require(wd_encode_completion_queue_push(&queue, 3), "push first completion");
    require(wd_encode_completion_queue_push(&queue, 1), "push second completion");

    uint16_t job = 0;
    require(wd_encode_completion_queue_pop(&queue, &job) && job == 3, "completion order preserves worker finish order");
    require(wd_encode_completion_queue_pop(&queue, &job) && job == 1, "second completion follows first");
    require(!wd_encode_completion_queue_pop(&queue, &job), "empty queue does not pop");
}

void test_wraparound_and_capacity() {
    uint16_t storage[3]{};
    wd_encode_completion_queue queue{};
    wd_encode_completion_queue_init(&queue, storage, 3);

    require(wd_encode_completion_queue_push(&queue, 0), "push zero");
    require(wd_encode_completion_queue_push(&queue, 1), "push one");
    require(wd_encode_completion_queue_push(&queue, 2), "push two");
    require(!wd_encode_completion_queue_push(&queue, 9), "full queue rejects overflow");

    uint16_t job = 0;
    require(wd_encode_completion_queue_pop(&queue, &job) && job == 0, "pop before wrap");
    require(wd_encode_completion_queue_push(&queue, 3), "write index wraps");

    require(wd_encode_completion_queue_pop(&queue, &job) && job == 1, "wrapped queue retains one");
    require(wd_encode_completion_queue_pop(&queue, &job) && job == 2, "wrapped queue retains two");
    require(wd_encode_completion_queue_pop(&queue, &job) && job == 3, "wrapped queue retains new item");
}

void test_invalid_storage_is_safe() {
    wd_encode_completion_queue queue{};
    wd_encode_completion_queue_init(&queue, nullptr, 0);

    uint16_t job = 0;
    require(!wd_encode_completion_queue_push(&queue, 1), "zero-capacity queue rejects push");
    require(!wd_encode_completion_queue_pop(&queue, &job), "zero-capacity queue rejects pop");
}

} // namespace

int main() {
    test_fifo_order();
    test_wraparound_and_capacity();
    test_invalid_storage_is_safe();
    return 0;
}
