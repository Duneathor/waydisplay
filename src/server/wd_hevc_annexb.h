#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Repair the observed VA-API HEVC packed-header defect: an emulation
 * prevention byte inserted inside a NAL start-code prefix. Operates only
 * when the access unit starts with an escaped prefix; never rewrites a
 * valid Annex-B packet. Returns the number of repaired prefixes, or -1 if
 * the packet cannot be recognized. The buffer is modified in place. */
int wd_hevc_annexb_repair_vaapi(uint8_t* data, size_t* size);

#ifdef __cplusplus
}
#endif
