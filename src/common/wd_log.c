#include "waydisplay/wd_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static const char* wd_log_level_name(enum wd_log_level level) {
    switch (level)
    {
    case WD_LOG_LEVEL_ERROR:
        return "error";
    case WD_LOG_LEVEL_WARN:
        return "warn";
    case WD_LOG_LEVEL_INFO:
        return "info";
    case WD_LOG_LEVEL_STATS:
        return "stats";
    case WD_LOG_LEVEL_DEBUG:
        return "debug";
    }

    return "log";
}

static bool wd_log_format_timestamp(char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0)
    {
        return false;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    {
        return false;
    }

    struct tm local_time;
    if (!localtime_r(&now.tv_sec, &local_time))
    {
        return false;
    }

    const size_t date_length = strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &local_time);
    if (date_length == 0 || date_length >= buffer_size)
    {
        return false;
    }

    const int written = snprintf(buffer + date_length, buffer_size - date_length, ".%03ld", now.tv_nsec / 1000000L);
    return written > 0 && (size_t)written < buffer_size - date_length;
}

void wd_log_message_va(enum wd_log_level level, const char* fmt, va_list args) {
    char       timestamp[32];
    const bool have_timestamp = wd_log_format_timestamp(timestamp, sizeof(timestamp));

    flockfile(stderr);
    if (have_timestamp)
    {
        fprintf(stderr, "[%s] WayDisplay [%s]: ", timestamp, wd_log_level_name(level));
    }
    else
    {
        fprintf(stderr, "WayDisplay [%s]: ", wd_log_level_name(level));
    }
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    funlockfile(stderr);
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
