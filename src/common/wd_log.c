#include "waydisplay/wd_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* wd_log_level_name(enum wd_log_level level) {
    switch (level)
    {
    case WD_LOG_LEVEL_ERROR:
        return "error";
    case WD_LOG_LEVEL_WARN:
        return "warn";
    case WD_LOG_LEVEL_INFO:
        return "info";
    case WD_LOG_LEVEL_DEBUG:
        return "debug";
    }

    return "log";
}

void wd_log_message_va(enum wd_log_level level, const char* fmt, va_list args) {
    fprintf(stderr, "WayDisplay [%s]: ", wd_log_level_name(level));
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

void wd_log_message(enum wd_log_level level, const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    wd_log_message_va(level, fmt, args);
    va_end(args);
}

bool wd_log_rate_limit_should_log(uint64_t* last_log_ns, uint64_t now_ns, uint64_t interval_ns) {
    if (!last_log_ns)
    {
        return true;
    }

    if (*last_log_ns == 0 || now_ns - *last_log_ns >= interval_ns)
    {
        *last_log_ns = now_ns;
        return true;
    }

    return false;
}
