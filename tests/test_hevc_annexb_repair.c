#include "wd_hevc_annexb.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    uint8_t packet[] = {
        0, 0, 3, 0, 1, 0x46, 0x01, 0x30, 0x10, 0x20,
        0, 0, 3, 0, 1, 0x02, 0x01, 0xd0, 0x09, 0xff,
        0, 0, 3, 0, 1, 0xfe, 0x00, 0x34, /* invalid NAL: must stay untouched */
    };
    const uint8_t expected[] = {
        0, 0, 0, 1, 0x46, 0x01, 0x30, 0x10, 0x20,
        0, 0, 0, 1, 0x02, 0x01, 0xd0, 0x09, 0xff,
        0, 0, 3, 0, 1, 0xfe, 0x00, 0x34,
    };
    size_t size = sizeof(packet);
    assert(wd_hevc_annexb_repair_vaapi(packet, &size) == 2);
    assert(size == sizeof(expected));
    assert(memcmp(packet, expected, size) == 0);
    assert(wd_hevc_annexb_repair_vaapi(packet, &size) == 0);
    assert(memcmp(packet, expected, size) == 0);

    uint8_t valid[] = {0, 0, 0, 1, 0x46, 0x01, 0, 0, 3, 0, 1, 0x02, 0x01};
    size = sizeof(valid);
    assert(wd_hevc_annexb_repair_vaapi(valid, &size) == 0);
    assert(size == sizeof(valid));
    assert(valid[8] == 3);

    uint8_t broken[] = {0, 0, 3, 0, 1, 0x80, 0x01};
    size = sizeof(broken);
    assert(wd_hevc_annexb_repair_vaapi(broken, &size) == -1);
    assert(size == sizeof(broken));

    uint8_t short_packet[] = {0, 0, 3, 0, 1};
    size = sizeof(short_packet);
    assert(wd_hevc_annexb_repair_vaapi(short_packet, &size) == -1);
    return 0;
}
