#pragma once

#include <cstdint>

namespace waydisplay {

enum class ClientVideoOutputStorage : uint8_t {
    CpuIYUV = 0,
    DrmPrime = 1,
};

struct ClientVideoOutputCapabilities {
    bool drm_prime_import = false;
    bool modifiers        = false;
};

constexpr ClientVideoOutputStorage client_video_output_storage(
    bool prefer_gpu_output, const ClientVideoOutputCapabilities& capabilities) {
    return prefer_gpu_output && capabilities.drm_prime_import
               ? ClientVideoOutputStorage::DrmPrime
               : ClientVideoOutputStorage::CpuIYUV;
}

} // namespace waydisplay
