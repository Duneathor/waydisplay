#include "waydisplay/wd_video_observation.h"
#include "test_check.h"
int main(void) {
    WD_TEST_CHECK(wd_video_rate_per_sec(1800, UINT64_C(60000000000)) == 30.0);
    WD_TEST_CHECK(wd_video_rate_per_sec(3600, UINT64_C(60000000000)) == 60.0);
    WD_TEST_CHECK(wd_video_rate_per_sec(1, 0) == 0.0);
    WD_TEST_CHECK(wd_video_payload_mbit_per_sec(125000, UINT64_C(1000000000)) == 1.0);
    WD_TEST_CHECK(wd_video_payload_mbit_per_sec(125000, 0) == 0.0);
    return 0;
}
