#include "wd_audio_routing.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool wd_audio_routing_identifier_valid(const char* value) {
    if (!value || value[0] == '\0')
    {
        return false;
    }
    for (const unsigned char* cursor = (const unsigned char*)value; *cursor != '\0'; ++cursor)
    {
        if (!(isalnum(*cursor) || *cursor == '.' || *cursor == '_' || *cursor == '-' || *cursor == ':'))
        {
            return false;
        }
    }
    return true;
}

static bool wd_audio_routing_copy(char* destination, size_t capacity, const char* source) {
    return snprintf(destination, capacity, "%s", source) >= 0 && strlen(source) < capacity;
}

bool wd_audio_routing_env_build(struct wd_audio_routing_env* env, const char* sink_name, const char* target, pid_t server_pid) {
    if (!env || server_pid <= 0)
    {
        return false;
    }
    memset(env, 0, sizeof(*env));

    if (!sink_name || sink_name[0] == '\0' || !target || target[0] == '\0')
    {
        return true;
    }
    if (!wd_audio_routing_identifier_valid(sink_name) || !wd_audio_routing_identifier_valid(target))
    {
        return false;
    }
    if (!wd_audio_routing_copy(env->pulse_sink, sizeof(env->pulse_sink), sink_name) ||
        !wd_audio_routing_copy(env->pipewire_target, sizeof(env->pipewire_target), target))
    {
        return false;
    }

    int written = snprintf(env->scope, sizeof(env->scope), "waydisplay.%ld", (long)server_pid);
    if (written < 0 || (size_t)written >= sizeof(env->scope))
    {
        return false;
    }

    /* PULSE_SINK supplies the playback target. PULSE_PROP adds a fail-closed
     * policy marker without assigning target.object to capture streams that
     * may be created by the same process. */
    written = snprintf(env->pulse_props, sizeof(env->pulse_props),
                       "node.dont-fallback=true "
                       "waydisplay.audio.scope=%s",
                       env->scope);
    if (written < 0 || (size_t)written >= sizeof(env->pulse_props))
    {
        return false;
    }

    /* Native PipeWire and PipeWire-ALSA consume these properties directly.
     * The explicit target plus dont-fallback prevents policy from silently
     * moving failed playback streams to the machine's physical sink. */
    written = snprintf(env->pipewire_props, sizeof(env->pipewire_props),
                       "{ target.object = \"%s\" "
                       "node.dont-fallback = true "
                       "node.dont-reconnect = true "
                       "node.dont-move = true "
                       "state.restore-props = false "
                       "waydisplay.audio.scope = \"%s\" }",
                       env->pipewire_target, env->scope);
    if (written < 0 || (size_t)written >= sizeof(env->pipewire_props))
    {
        return false;
    }

    env->enabled = true;
    return true;
}
