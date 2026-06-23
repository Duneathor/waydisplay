#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_AUDIO_ROUTING_SINK_MAX           96u
#define WD_AUDIO_ROUTING_TARGET_MAX         96u
#define WD_AUDIO_ROUTING_SCOPE_MAX          64u
#define WD_AUDIO_ROUTING_PULSE_PROPS_MAX    256u
#define WD_AUDIO_ROUTING_PIPEWIRE_PROPS_MAX 512u

struct wd_audio_routing_env {
    bool enabled;
    char pulse_sink[WD_AUDIO_ROUTING_SINK_MAX];
    char pipewire_target[WD_AUDIO_ROUTING_TARGET_MAX];
    char scope[WD_AUDIO_ROUTING_SCOPE_MAX];
    char pulse_props[WD_AUDIO_ROUTING_PULSE_PROPS_MAX];
    char pipewire_props[WD_AUDIO_ROUTING_PIPEWIRE_PROPS_MAX];
};

/* Build process-scoped routing values for the server-launched application tree.
 * The private sink remains a non-default graph node; only descendants that
 * inherit these values request it as their playback target. */
bool wd_audio_routing_env_build(struct wd_audio_routing_env* env, const char* sink_name, const char* target, pid_t server_pid);

#ifdef __cplusplus
}
#endif
