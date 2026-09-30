#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace waydisplay {

class ClientRenderWake {
  public:
    uint64_t sequence() const;
    void     signal();
    bool     wait_for_change(uint64_t observed_sequence, uint32_t timeout_ms);
    void     set_external_waker(std::function<void()> waker);
    void     clear_external_waker();

  private:
    std::atomic<uint64_t>   sequence_{1};
    std::mutex              mutex_;
    std::condition_variable condition_;
    std::function<void()>   external_waker_;
};

} // namespace waydisplay
