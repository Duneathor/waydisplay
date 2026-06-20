#include "render_wakeup.hpp"

#include <chrono>

namespace waydisplay {

uint64_t ClientRenderWake::sequence() const {
    return sequence_.load(std::memory_order_acquire);
}

void ClientRenderWake::signal() {
    sequence_.fetch_add(1, std::memory_order_release);
    condition_.notify_one();
}

bool ClientRenderWake::wait_for_change(uint64_t observed_sequence, uint32_t timeout_ms) {
    if (sequence() != observed_sequence)
    {
        return true;
    }
    if (timeout_ms == 0)
    {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
        return sequence_.load(std::memory_order_acquire) != observed_sequence;
    });
}

} // namespace waydisplay
