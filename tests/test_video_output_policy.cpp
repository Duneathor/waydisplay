#include "video_output_policy.hpp"

#include <stdio.h>

using namespace waydisplay;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    /* SDL-style CPU presenter: decoder must never emit DRM-PRIME merely
     * because hardware decode is available. */
    ClientVideoOutputCapabilities none{};
    CHECK(client_video_output_storage(false, none) == ClientVideoOutputStorage::CpuIYUV);
    CHECK(client_video_output_storage(true, none) == ClientVideoOutputStorage::CpuIYUV);

    ClientVideoOutputCapabilities dmabuf{};
    dmabuf.drm_prime_import = true;
    CHECK(client_video_output_storage(false, dmabuf) == ClientVideoOutputStorage::CpuIYUV);
    CHECK(client_video_output_storage(true, dmabuf) == ClientVideoOutputStorage::CpuIYUV);

    dmabuf.modifiers = true;
    CHECK(client_video_output_storage(true, dmabuf) == ClientVideoOutputStorage::DrmPrime);

    puts("video output policy: PASS");
    return 0;
}
