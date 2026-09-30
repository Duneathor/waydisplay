#include "wd_hevc_annexb.h"

#include <string.h>

/* Only consider emulation-escaped prefixes followed by a plausible HEVC
 * NAL header. In particular, the nuh_temporal_id_plus1 field must be nonzero
 * and the forbidden_zero_bit must be clear. This is a narrow workaround for
 * the driver output seen in the server trace, not a general RBSP unescaper. */
static int wd_hevc_nal_header(const uint8_t* bytes, size_t available) {
    /* WayDisplay encodes base-layer pictures only. Reject apparent NAL
     * headers with nonzero layer IDs to reduce false positives inside RBSP. */
    if (available < 2 || (bytes[0] & 0x81u) != 0 || (bytes[1] & 0xf8u) != 0 || (bytes[1] & 0x07u) == 0)
    {
        return 0;
    }
    const unsigned type = (unsigned)(bytes[0] >> 1u) & 0x3fu;
    return type <= 40u;
}

static size_t wd_escaped_prefix_length(const uint8_t* data, size_t len) {
    if (len >= 7 && data[0] == 0 && data[1] == 0 && data[2] == 3 && data[3] == 0 && data[4] == 1 &&
        wd_hevc_nal_header(data + 5, len - 5))
    {
        return 5;
    }
    if (len >= 6 && data[0] == 0 && data[1] == 0 && data[2] == 3 && data[3] == 1 &&
        wd_hevc_nal_header(data + 4, len - 4))
    {
        return 4;
    }
    return 0;
}

int wd_hevc_annexb_repair_vaapi(uint8_t* data, size_t* size) {
    if (!data || !size || *size < 6)
    {
        return -1;
    }
    const size_t original = *size;
    if (data[0] == 0 && data[1] == 0 && (data[2] == 1 || (original >= 4 && data[2] == 0 && data[3] == 1)))
    {
        return 0;
    }

    const size_t escaped = wd_escaped_prefix_length(data, original);
    if (!escaped)
    {
        return -1;
    }

    /* Once inside an HEVC NAL payload, 00 00 03 ... is legitimate emulation
     * prevention. A header-looking byte pair after such a sequence is not
     * enough to prove another malformed NAL boundary. If an additional
     * escaped-prefix candidate exists, the packet is ambiguous: reject it
     * rather than rewriting compressed payload bytes into a fake delimiter. */
    for (size_t pos = escaped; pos < original; ++pos)
    {
        if (wd_escaped_prefix_length(data + pos, original - pos) != 0)
        {
            return -1;
        }
    }

    const size_t repaired_prefix = escaped - 1u;
    data[0] = 0;
    data[1] = 0;
    if (repaired_prefix == 4)
    {
        data[2] = 0;
        data[3] = 1;
    }
    else
    {
        data[2] = 1;
    }
    memmove(data + repaired_prefix, data + escaped, original - escaped);
    *size = original - 1u;
    return 1;
}
