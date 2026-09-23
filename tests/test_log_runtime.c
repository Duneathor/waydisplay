#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "waydisplay/wd_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    FILE* capture = tmpfile();
    assert(capture);
    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);

    int side_effects = 0;
    assert(!wd_log_is_verbose());
    assert(!wd_log_would_log(WD_LOG_LEVEL_WARN));
    assert(wd_log_would_log(WD_LOG_LEVEL_ERROR));
    WD_LOG_WARN("suppressed-warn %d", ++side_effects);
    WD_LOG_INFO("suppressed-info %d", ++side_effects);
    WD_LOG_STATS("suppressed-stats %d", ++side_effects);
    WD_LOG_DEBUG("suppressed-debug %d", ++side_effects);
    wd_log_message(WD_LOG_LEVEL_INFO, "suppressed-direct");
    assert(side_effects == 0);
    WD_LOG_ERROR("visible-error");

    wd_log_set_verbose(true);
#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_INFO
    assert(wd_log_is_verbose());
    WD_LOG_INFO("visible-info %d", ++side_effects);
    WD_LOG_WARN("visible-warn");
    assert(side_effects == 1);
#endif
    wd_log_set_verbose(false);
    assert(!wd_log_is_verbose());
    assert(!wd_log_would_log(WD_LOG_LEVEL_INFO));
    WD_LOG_INFO("suppressed-again %d", ++side_effects);
    assert(side_effects == 1);

    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    char output[1024] = {0};
    const size_t bytes = fread(output, 1, sizeof(output) - 1, capture);
    assert(bytes > 0 && bytes < sizeof(output));
    assert(strstr(output, "visible-error"));
    assert(!strstr(output, "suppressed-"));
#if WAYDISPLAY_LOG_LEVEL >= WD_LOG_LEVEL_VALUE_INFO
    assert(strstr(output, "visible-info 1"));
    assert(strstr(output, "visible-warn"));
#endif
    fclose(capture);
    return 0;
}
