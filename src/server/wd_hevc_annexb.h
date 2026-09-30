#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Repair the observed VA-API HEVC packed-header defect: an emulation
 * prevention byte inserted inside the leading NAL start-code prefix.
 *
 * The caller must provide writable storage. The function never grows the
 * packet; on a successful repair *size decreases by one and bytes beyond the
 * returned *size are unspecified. A packet that begins with valid Annex-B is
 * left untouched. If another escaped-prefix candidate appears later, the
 * packet is ambiguous with legitimate RBSP emulation prevention and is
 * rejected rather than rewriting interior payload bytes.
 *
 * Returns 1 when the leading prefix was repaired, 0 when already valid, or -1
 * when the packet cannot be repaired unambiguously. */
int wd_hevc_annexb_repair_vaapi(uint8_t* data, size_t* size);

#ifdef __cplusplus
}
#endif
