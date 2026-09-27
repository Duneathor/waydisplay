#include "wd_video_encoder.h"
#include "waydisplay/wd_frame.h"

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    struct wd_video_encoder* encoder = NULL;
    CHECK(wd_video_encoder_create(&encoder, "software"));
    CHECK(encoder != NULL);
    CHECK(!wd_video_encoder_supports_drm_prime(encoder));

    struct wd_frame invalid;
    wd_frame_init(&invalid);
    struct wd_video_encoder_packet packet = {0};
    CHECK(!wd_video_encoder_encode_frame(encoder, &invalid, &packet));

    wd_video_encoder_destroy(encoder);
    puts("video encoder frame API: PASS");
    return 0;
}
