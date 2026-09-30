#include "video_av1_tiles.h"

#include "test_check.h"
#include <string.h>

int main(void) {
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(66, 50), "1x1") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(767, 720), "1x1") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(768, 431), "1x1") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(768, 432), "2x1") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(1279, 720), "2x1") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(1280, 719), "2x1") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(1280, 720), "2x2") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(1422, 773), "2x2") == 0);
    WD_TEST_CHECK(strcmp(wd_video_av1_software_tiles(1920, 1080), "2x2") == 0);
    return 0;
}
