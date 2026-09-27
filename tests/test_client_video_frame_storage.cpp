#include "video_decoder.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <utility>

using waydisplay::ClientVideoFrameBuffer;
using waydisplay::ClientVideoPixelFormat;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);

    ClientVideoFrameBuffer frame;
    struct wd_frame_drm_plane plane{pipefd[0], 256, 0, 0};
    CHECK(wd_frame_set_drm_prime_dup(&frame.gpu_frame, 64, 32, 0x3231564eu, 1, &plane, 1));
    close(pipefd[0]);
    close(pipefd[1]);
    frame.format = ClientVideoPixelFormat::DRMPrime;
    frame.width = 64;
    frame.height = 32;
    CHECK(frame.gpu_valid());
    const int owned_fd = frame.gpu_frame.data.drm.planes[0].fd;

    ClientVideoFrameBuffer moved = std::move(frame);
    CHECK(!frame.valid());
    CHECK(moved.gpu_valid());
    CHECK(fcntl(owned_fd, F_GETFD) >= 0);

    moved.clear();
    CHECK(fcntl(owned_fd, F_GETFD) < 0);
    CHECK(!moved.valid());

    puts("client video frame storage: PASS");
    return 0;
}
