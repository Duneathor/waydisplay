#include "waydisplay/wd_log.h"

#include <stdio.h>

static const char* wd_log_level_name(enum wd_log_level level) {
    switch (level)
    {
    case WD_LOG_LEVEL_ERROR:
        return "error";
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
