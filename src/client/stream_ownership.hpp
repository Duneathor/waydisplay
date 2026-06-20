#pragma once

#include <atomic>
#include <cstdint>

namespace waydisplay {

enum class ClientContentOwner : uint8_t {
    Tiles = 0,
    Video = 1,
};

struct ClientContentOwnershipSnapshot {
    uint64_t epoch = 0;
    ClientContentOwner owner = ClientContentOwner::Tiles;
};

class ClientStreamOwnership {
  public:
    ClientContentOwnershipSnapshot snapshot() const;
    uint64_t begin_video_frame();
    uint64_t end_video_stream();
    uint64_t reset_to_tiles();
    bool is_current(uint64_t epoch, ClientContentOwner owner) const;

  private:
    uint64_t advance(ClientContentOwner owner);

    static constexpr uint64_t pack(uint64_t epoch, ClientContentOwner owner) {
        return (epoch << 1u) | static_cast<uint64_t>(owner);
    }

    std::atomic<uint64_t> state_{pack(1, ClientContentOwner::Tiles)};
};

} // namespace waydisplay
