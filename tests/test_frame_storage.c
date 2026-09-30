#include "waydisplay/wd_frame.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    struct wd_frame frame;
    struct wd_frame clone;
    wd_frame_init(&frame);
    wd_frame_init(&clone);

    struct wd_buffer* pixels = wd_buffer_alloc(4u * 4u * 4u);
    CHECK(pixels != NULL);
    memset(wd_buffer_data(pixels), 0x5a, wd_buffer_size(pixels));
    CHECK(wd_frame_set_cpu_xrgb8888(&frame, pixels, 0, 4, 4, 16, 0x34325258u, 99));
    wd_buffer_release(pixels);
    CHECK(wd_frame_valid(&frame));
    CHECK(wd_frame_cpu_data(&frame)[0] == 0x5a);
    CHECK(wd_frame_clone(&clone, &frame));
    wd_frame_reset(&frame);
    CHECK(wd_frame_valid(&clone));
    CHECK(wd_frame_cpu_data(&clone)[63] == 0x5a);
    wd_frame_reset(&clone);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    struct wd_frame_drm_plane plane = {
        .fd = pipefd[0], .stride = 256, .offset = 32, .modifier = 0x1122334455667788ull
    };
    CHECK(wd_frame_set_drm_prime_dup(&frame, 64, 32, 0x34325258u, 1234, &plane, 1));
    close(pipefd[0]);
    close(pipefd[1]);
    CHECK(wd_frame_valid(&frame));
    CHECK(frame.data.drm.planes[0].fd >= 0);
    CHECK((fcntl(frame.data.drm.planes[0].fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(wd_frame_clone(&clone, &frame));
    CHECK(clone.data.drm.planes[0].fd != frame.data.drm.planes[0].fd);
    wd_frame_reset(&frame);
    CHECK(wd_frame_valid(&clone));
    CHECK((fcntl(clone.data.drm.planes[0].fd, F_GETFD) & FD_CLOEXEC) != 0);
    wd_frame_reset(&clone);

    CHECK(!wd_frame_valid(&clone));
    puts("wd_frame ownership: PASS");
    return 0;
}
