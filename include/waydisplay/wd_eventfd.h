#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signal a Linux eventfd once. The helper is lock-free and retries EINTR. */
bool wd_eventfd_signal(int fd);

#ifdef __cplusplus
}
#endif
