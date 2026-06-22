#pragma once

/* Internal FFmpeg VAAPI device discovery.  This header is included only by
 * codec translation units that already include libavutil/hwcontext.h. */

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool wd_vaapi_render_node_path_is_valid(const char* path) {
    if (!path)
    {
        return false;
    }

    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    static const char prefix[] = "renderD";
    if (strncmp(name, prefix, sizeof(prefix) - 1u) != 0)
    {
        return false;
    }

    name += sizeof(prefix) - 1u;
    if (*name == '\0')
    {
        return false;
    }
    for (; *name != '\0'; ++name)
    {
        if (!isdigit((unsigned char)*name))
        {
            return false;
        }
    }
    return true;
}

static int wd_vaapi_open_automatic_device(AVBufferRef** out_device,
                                          char* selected_path,
                                          size_t selected_path_size) {
    if (!out_device)
    {
        return AVERROR(EINVAL);
    }

    *out_device = NULL;
    if (selected_path && selected_path_size != 0)
    {
        selected_path[0] = '\0';
    }

    int last_error = AVERROR(ENODEV);
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    const int glob_result = glob("/dev/dri/renderD*", 0, NULL, &matches);
    if (glob_result == 0)
    {
        for (size_t index = 0; index < matches.gl_pathc; ++index)
        {
            const char* candidate = matches.gl_pathv[index];
            if (!wd_vaapi_render_node_path_is_valid(candidate))
            {
                continue;
            }

            AVBufferRef* device = NULL;
            const int rc = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI,
                                                   candidate, NULL, 0);
            if (rc >= 0)
            {
                *out_device = device;
                if (selected_path && selected_path_size != 0)
                {
                    (void)snprintf(selected_path, selected_path_size, "%s", candidate);
                }
                globfree(&matches);
                return 0;
            }
            av_buffer_unref(&device);
            last_error = rc;
        }
    }
    globfree(&matches);

    /* Preserve FFmpeg/libva's platform-specific automatic fallback for hosts
     * that expose a VA display without a conventional DRM render-node path. */
    AVBufferRef* automatic_device = NULL;
    const int automatic_rc = av_hwdevice_ctx_create(
        &automatic_device, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (automatic_rc >= 0)
    {
        *out_device = automatic_device;
        if (selected_path && selected_path_size != 0)
        {
            (void)snprintf(selected_path, selected_path_size, "%s", "automatic");
        }
        return 0;
    }
    av_buffer_unref(&automatic_device);
    return automatic_rc < 0 ? automatic_rc : last_error;
}
