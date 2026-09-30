#include "wd_hevc_annexb.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    uint8_t single[] = {0, 0, 3, 0, 1, 0x46, 0x01, 0x30, 0x10, 0x20};
    const uint8_t single_expected[] = {0, 0, 0, 1, 0x46, 0x01, 0x30, 0x10, 0x20};
    size_t size = sizeof(single);
    assert(wd_hevc_annexb_repair_vaapi(single, &size) == 1);
    assert(size == sizeof(single_expected));
    assert(memcmp(single, single_expected, size) == 0);
    assert(wd_hevc_annexb_repair_vaapi(single, &size) == 0);

    /* More than one escaped-prefix candidate is ambiguous: an interior
     * 00 00 03 00 01 sequence can be legitimate RBSP emulation-prevention
     * data. Fail closed without changing the packet. */
    uint8_t ambiguous[] = {
        0, 0, 3, 0, 1, 0x46, 0x01, 0x30,
        0, 0, 3, 0, 1, 0x02, 0x01, 0xd0,
    };
    uint8_t ambiguous_before[sizeof(ambiguous)];
    memcpy(ambiguous_before, ambiguous, sizeof(ambiguous));
    size = sizeof(ambiguous);
    assert(wd_hevc_annexb_repair_vaapi(ambiguous, &size) == -1);
    assert(size == sizeof(ambiguous));
    assert(memcmp(ambiguous, ambiguous_before, sizeof(ambiguous)) == 0);

    uint8_t valid[] = {0, 0, 0, 1, 0x46, 0x01, 0, 0, 3, 0, 1, 0x02, 0x01};
    uint8_t valid_before[sizeof(valid)];
    memcpy(valid_before, valid, sizeof(valid));
    size = sizeof(valid);
    assert(wd_hevc_annexb_repair_vaapi(valid, &size) == 0);
    assert(size == sizeof(valid));
    assert(memcmp(valid, valid_before, sizeof(valid)) == 0);

    uint8_t invalid_nal[] = {0, 0, 3, 0, 1, 0x80, 0x01};
    size = sizeof(invalid_nal);
    assert(wd_hevc_annexb_repair_vaapi(invalid_nal, &size) == -1);
    assert(size == sizeof(invalid_nal));

    uint8_t short_packet[] = {0, 0, 3, 0, 1};
    size = sizeof(short_packet);
    assert(wd_hevc_annexb_repair_vaapi(short_packet, &size) == -1);
    return 0;
}
