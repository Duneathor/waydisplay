#pragma once

#include <stdint.h>

#define WD_USEC_PER_MSEC 1000ull
#define WD_MSEC_PER_SEC  1000ull
#define WD_NSEC_PER_USEC 1000ull
#define WD_NSEC_PER_MSEC (WD_NSEC_PER_USEC * WD_USEC_PER_MSEC)
#define WD_NSEC_PER_SEC  (WD_NSEC_PER_MSEC * WD_MSEC_PER_SEC)

#ifdef __cplusplus
extern "C" {
#endif

uint64_t wd_now_ns(void);
uint32_t wd_now_ms32(void);
void     wd_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
