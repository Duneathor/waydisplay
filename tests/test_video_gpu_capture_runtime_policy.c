#include "video_gpu_capture_runtime_policy.h"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    CHECK(!wd_video_gpu_capture_backoff_active(100, 0));
    CHECK(!wd_video_gpu_capture_backoff_active(100, 100));
    CHECK(wd_video_gpu_capture_backoff_active(99, 100));

    CHECK(wd_video_gpu_capture_backoff_deadline(10) ==
          10 + WD_VIDEO_GPU_CAPTURE_BACKOFF_NS);
    CHECK(wd_video_gpu_capture_backoff_deadline(
              UINT64_MAX - WD_VIDEO_GPU_CAPTURE_BACKOFF_NS + 1) == UINT64_MAX);

    return 0;
}
