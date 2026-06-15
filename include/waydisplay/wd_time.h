#pragma once

#include <stdint.h>

#define WD_NSEC_PER_SEC 1000000000ull

#ifdef __cplusplus
extern "C" {
#endif

uint64_t wd_now_ns(void);
uint32_t wd_now_ms32(void);
void     wd_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
