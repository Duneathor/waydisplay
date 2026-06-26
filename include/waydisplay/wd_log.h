#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define WD_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define WD_PRINTF_FORMAT(fmt_index, first_arg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WD_LOG_LEVEL_VALUE_OFF   (-1)
#define WD_LOG_LEVEL_VALUE_ERROR 0
#define WD_LOG_LEVEL_VALUE_WARN  1
#define WD_LOG_LEVEL_VALUE_INFO  2
#define WD_LOG_LEVEL_VALUE_STATS 3
#define WD_LOG_LEVEL_VALUE_DEBUG 4

enum wd_log_level {
    WD_LOG_LEVEL_ERROR = WD_LOG_LEVEL_VALUE_ERROR,
    WD_LOG_LEVEL_WARN  = WD_LOG_LEVEL_VALUE_WARN,
    WD_LOG_LEVEL_INFO  = WD_LOG_LEVEL_VALUE_INFO,
    WD_LOG_LEVEL_STATS = WD_LOG_LEVEL_VALUE_STATS,
    WD_LOG_LEVEL_DEBUG = WD_LOG_LEVEL_VALUE_DEBUG,
};

void wd_log_message(enum wd_log_level level, const char* fmt, ...) WD_PRINTF_FORMAT(2, 3);
void wd_log_message_va(enum wd_log_level level, const char* fmt, va_list args) WD_PRINTF_FORMAT(2, 0);
bool wd_log_rate_limit_should_log(uint64_t* last_log_ns, uint64_t now_ns, uint64_t interval_ns);

#ifndef WAYDISPLAY_LOG_LEVEL
#define WAYDISPLAY_LOG_LEVEL WD_LOG_LEVEL_VALUE_INFO
#endif

#if WAYDISPLAY_LOG_LEVEL < WD_LOG_LEVEL_VALUE_OFF || WAYDISPLAY_LOG_LEVEL > WD_LOG_LEVEL_VALUE_DEBUG
#error "WAYDISPLAY_LOG_LEVEL must be between OFF (-1) and DEBUG (4)"
#endif

#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_ERROR
#define WD_LOG_ERROR(...) wd_log_message(WD_LOG_LEVEL_ERROR, __VA_ARGS__)
#else
#define WD_LOG_ERROR(...) ((void)0)
#endif

#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_WARN
#define WD_LOG_WARN(...) wd_log_message(WD_LOG_LEVEL_WARN, __VA_ARGS__)
#else
#define WD_LOG_WARN(...) ((void)0)
#endif

#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_INFO
#define WD_LOG_INFO(...) wd_log_message(WD_LOG_LEVEL_INFO, __VA_ARGS__)
#else
#define WD_LOG_INFO(...) ((void)0)
#endif

#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_STATS
#define WD_LOG_STATS(...) wd_log_message(WD_LOG_LEVEL_STATS, __VA_ARGS__)
#else
#define WD_LOG_STATS(...) ((void)0)
#endif

#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_DEBUG
#define WD_LOG_DEBUG(...) wd_log_message(WD_LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#define WD_LOG_DEBUG(...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#undef WD_PRINTF_FORMAT
