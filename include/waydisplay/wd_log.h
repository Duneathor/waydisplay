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

enum wd_log_level {
    WD_LOG_LEVEL_ERROR = 0,
    WD_LOG_LEVEL_WARN  = 1,
    WD_LOG_LEVEL_INFO  = 2,
    WD_LOG_LEVEL_DEBUG = 3,
};

void wd_log_message(enum wd_log_level level, const char* fmt, ...) WD_PRINTF_FORMAT(2, 3);
void wd_log_message_va(enum wd_log_level level, const char* fmt, va_list args) WD_PRINTF_FORMAT(2, 0);
bool wd_log_rate_limit_should_log(uint64_t* last_log_ns, uint64_t now_ns, uint64_t interval_ns);

#ifndef WAYDISPLAY_ENABLE_LOGGING
#define WAYDISPLAY_ENABLE_LOGGING 1
#endif

#ifndef WAYDISPLAY_ENABLE_DEBUG_LOGGING
#define WAYDISPLAY_ENABLE_DEBUG_LOGGING 0
#endif

#if WAYDISPLAY_ENABLE_LOGGING
#define WD_LOG_ERROR(...) wd_log_message(WD_LOG_LEVEL_ERROR, __VA_ARGS__)
#define WD_LOG_WARN(...)  wd_log_message(WD_LOG_LEVEL_WARN, __VA_ARGS__)
#define WD_LOG_INFO(...)  wd_log_message(WD_LOG_LEVEL_INFO, __VA_ARGS__)
#else
#define WD_LOG_ERROR(...) ((void)0)
#define WD_LOG_WARN(...)  ((void)0)
#define WD_LOG_INFO(...)  ((void)0)
#endif

#if WAYDISPLAY_ENABLE_LOGGING && WAYDISPLAY_ENABLE_DEBUG_LOGGING
#define WD_LOG_DEBUG(...) wd_log_message(WD_LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#define WD_LOG_DEBUG(...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#undef WD_PRINTF_FORMAT
