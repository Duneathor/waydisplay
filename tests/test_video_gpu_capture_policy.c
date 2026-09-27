#include "video_gpu_capture_policy.h"

#include <stdio.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    int fd[2];
    CHECK(pipe(fd) == 0);

    struct wd_frame frame;
    wd_frame_init(&frame);
    struct wd_frame_drm_plane plane = {
        .fd = fd[0], .stride = 256, .offset = 0, .modifier = 0
    };
    CHECK(wd_frame_set_drm_prime_dup(&frame, 64, 32, WD_DRM_FOURCC_XRGB8888, 0,
                                     &plane, 1));
    close(fd[0]);
    close(fd[1]);

    CHECK(wd_video_gpu_capture_frame_eligible(&frame, 64, 32));
    CHECK(!wd_video_gpu_capture_frame_eligible(&frame, 63, 32));
    CHECK(!wd_video_gpu_capture_frame_eligible(&frame, 64, 31));

    frame.fourcc = 0x3231564eu; /* NV12 is not the wlroots RGB capture contract. */
    CHECK(!wd_video_gpu_capture_frame_eligible(&frame, 64, 32));
    frame.fourcc = WD_DRM_FOURCC_ARGB8888;
    CHECK(wd_video_gpu_capture_frame_eligible(&frame, 64, 32));

    frame.data.drm.planes[0].stride = 255;
    CHECK(!wd_video_gpu_capture_frame_eligible(&frame, 64, 32));

    wd_frame_reset(&frame);
    CHECK(!wd_video_gpu_capture_frame_eligible(&frame, 64, 32));
    puts("video GPU capture policy: PASS");
    return 0;
}
