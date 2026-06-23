#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace waydisplay {

class ClientRenderWake {
  public:
    uint64_t sequence() const;
    void     signal();
    bool     wait_for_change(uint64_t observed_sequence, uint32_t timeout_ms);

  private:
    std::atomic<uint64_t>   sequence_{1};
    std::mutex              mutex_;
    std::condition_variable condition_;
};

} // namespace waydisplay
