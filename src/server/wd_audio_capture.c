#include "wd_audio_capture.h"
#include "waydisplay/wd_protocol.h"

#include "waydisplay/wd_log.h"
#include "waydisplay/wd_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE
#define WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE 0
#endif

#if WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

struct wd_audio_capture_port {
    uint8_t channel;
};

struct wd_audio_capture {
    struct pw_thread_loop* loop;
    struct pw_filter* filter;
    void* ports[WD_AUDIO_CHANNELS_MAX];
    wd_audio_capture_callback callback;
    void* userdata;
    uint8_t channels;
    bool ready;
    bool failed;
    bool delivery_enabled;
    char sink_name[96];
    char sink_target[32];
};

static void wd_audio_capture_process(void* userdata, struct spa_io_position* position) {
    struct wd_audio_capture* capture = userdata;
    if (!capture || !position || !__atomic_load_n(&capture->delivery_enabled, __ATOMIC_ACQUIRE))
    {
        return;
    }

    const uint32_t frames = position->clock.duration;
    if (frames == 0)
    {
        return;
    }

    const float* planes[WD_AUDIO_CHANNELS_MAX] = {0};
    for (uint8_t channel = 0; channel < capture->channels; ++channel)
    {
        planes[channel] = pw_filter_get_dsp_buffer(capture->ports[channel], frames);
        if (!planes[channel])
        {
            return;
        }
    }

    capture->callback(capture->userdata, planes, frames, capture->channels, wd_now_ns());
}

static void wd_audio_capture_state_changed(void* userdata, enum pw_filter_state old_state,
                                           enum pw_filter_state state, const char* error) {
    (void)old_state;
    struct wd_audio_capture* capture = userdata;
    if (!capture)
    {
        return;
    }
    if (state == PW_FILTER_STATE_ERROR)
    {
        capture->failed = true;
        WD_LOG_ERROR("PipeWire private audio sink error: %s", error ? error : "unknown");
    }
    else if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING)
    {
        capture->ready = true;
    }
    pw_thread_loop_signal(capture->loop, false);
}

static const struct pw_filter_events wd_audio_capture_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = wd_audio_capture_state_changed,
    .process = wd_audio_capture_process,
};

static void wd_audio_capture_cleanup(struct wd_audio_capture* capture) {
    if (!capture)
    {
        return;
    }
    if (capture->loop)
    {
        pw_thread_loop_lock(capture->loop);
        if (capture->filter)
        {
            pw_filter_destroy(capture->filter);
            capture->filter = NULL;
        }
        pw_thread_loop_unlock(capture->loop);
        pw_thread_loop_stop(capture->loop);
        pw_thread_loop_destroy(capture->loop);
        capture->loop = NULL;
    }
}

bool wd_audio_capture_create(struct wd_audio_capture** out_capture, uint8_t channels,
                             wd_audio_capture_callback callback, void* userdata) {
    if (!out_capture || !callback || channels == 0 || channels > WD_AUDIO_CHANNELS_MAX)
    {
        return false;
    }
    *out_capture = NULL;
    pw_init(NULL, NULL);

    struct wd_audio_capture* capture = calloc(1, sizeof(*capture));
    if (!capture)
    {
        return false;
    }
    capture->callback = callback;
    capture->userdata = userdata;
    capture->channels = channels;
    snprintf(capture->sink_name, sizeof(capture->sink_name),
             "waydisplay.audio.sink.%ld", (long)getpid());

    capture->loop = pw_thread_loop_new("waydisplay-audio-sink", NULL);
    if (!capture->loop)
    {
        free(capture);
        return false;
    }

    pw_thread_loop_lock(capture->loop);
    if (pw_thread_loop_start(capture->loop) < 0)
    {
        pw_thread_loop_unlock(capture->loop);
        pw_thread_loop_destroy(capture->loop);
        free(capture);
        return false;
    }

    char channel_count[8];
    snprintf(channel_count, sizeof(channel_count), "%u", channels);
    capture->filter = pw_filter_new_simple(
        pw_thread_loop_get_loop(capture->loop),
        "WayDisplay private audio sink",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Sink",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_MEDIA_CLASS, "Audio/Sink",
            PW_KEY_NODE_NAME, capture->sink_name,
            PW_KEY_NODE_DESCRIPTION, "WayDisplay private audio sink",
            "node.virtual", "true",
            "node.terminal", "true",
            "node.always-process", "true",
            "audio.rate", "48000",
            "audio.channels", channel_count,
            "audio.position", channels == 1 ? "[ MONO ]" : "[ FL FR ]",
            NULL),
        &wd_audio_capture_events, capture);
    if (!capture->filter)
    {
        pw_thread_loop_unlock(capture->loop);
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    static const char* channel_names[WD_AUDIO_CHANNELS_MAX] = {"FL", "FR"};
    for (uint8_t channel = 0; channel < channels; ++channel)
    {
        char port_name[24];
        snprintf(port_name, sizeof(port_name), "input_%s", channel_names[channel]);
        capture->ports[channel] = pw_filter_add_port(
            capture->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(struct wd_audio_capture_port),
            pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, port_name,
                "audio.channel", channels == 1 ? "MONO" : channel_names[channel],
                NULL),
            NULL, 0);
        if (!capture->ports[channel])
        {
            pw_thread_loop_unlock(capture->loop);
            wd_audio_capture_cleanup(capture);
            free(capture);
            return false;
        }
        ((struct wd_audio_capture_port*)capture->ports[channel])->channel = channel;
    }

    if (pw_filter_connect(capture->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0)
    {
        pw_thread_loop_unlock(capture->loop);
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    while (!capture->ready && !capture->failed)
    {
        const int wait_result = pw_thread_loop_timed_wait(capture->loop, 3);
        if (wait_result != 0)
        {
            WD_LOG_ERROR("timed out waiting for PipeWire private audio sink");
            capture->failed = true;
            break;
        }
    }
    pw_thread_loop_unlock(capture->loop);

    if (!capture->ready || capture->failed)
    {
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    /* proxy_bound_props() updates the filter properties before the PAUSED
     * state is emitted. Prefer object.serial for client routing: unlike a
     * generated node name it is the exact global object identity understood
     * by target.object/PIPEWIRE_NODE. */
    const struct pw_properties* bound_props =
        pw_filter_get_properties(capture->filter, NULL);
    const char* object_serial = bound_props
        ? pw_properties_get(bound_props, PW_KEY_OBJECT_SERIAL)
        : NULL;
    if (!object_serial || object_serial[0] == '\0' ||
        snprintf(capture->sink_target, sizeof(capture->sink_target),
                 "%s", object_serial) >= (int)sizeof(capture->sink_target))
    {
        WD_LOG_ERROR("PipeWire private audio sink has no usable object.serial");
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    WD_LOG_INFO("private audio sink ready: node=%s serial=%s rate=%u channels=%u",
                capture->sink_name, capture->sink_target,
                WD_AUDIO_SAMPLE_RATE_DEFAULT, channels);
    *out_capture = capture;
    return true;
}

void wd_audio_capture_destroy(struct wd_audio_capture* capture) {
    if (!capture)
    {
        return;
    }
    __atomic_store_n(&capture->delivery_enabled, false, __ATOMIC_RELEASE);
    wd_audio_capture_cleanup(capture);
    free(capture);
}

bool wd_audio_capture_start(struct wd_audio_capture* capture) {
    if (!capture || !capture->ready || capture->failed)
    {
        return false;
    }
    __atomic_store_n(&capture->delivery_enabled, true, __ATOMIC_RELEASE);
    return true;
}

void wd_audio_capture_stop(struct wd_audio_capture* capture) {
    if (capture)
    {
        __atomic_store_n(&capture->delivery_enabled, false, __ATOMIC_RELEASE);
    }
}

bool wd_audio_capture_available(void) {
    return true;
}

const char* wd_audio_capture_backend_name(void) {
    return "pipewire-private-sink";
}

const char* wd_audio_capture_sink_name(const struct wd_audio_capture* capture) {
    return capture ? capture->sink_name : NULL;
}

const char* wd_audio_capture_sink_target(const struct wd_audio_capture* capture) {
    return capture ? capture->sink_target : NULL;
}

#else

struct wd_audio_capture_port {
    uint8_t channel;
};

struct wd_audio_capture {
    int unused;
};

bool wd_audio_capture_create(struct wd_audio_capture** out_capture, uint8_t channels,
                             wd_audio_capture_callback callback, void* userdata) {
    (void)channels;
    (void)callback;
    (void)userdata;
    if (out_capture)
    {
        *out_capture = NULL;
    }
    return false;
}

void wd_audio_capture_destroy(struct wd_audio_capture* capture) {
    free(capture);
}

bool wd_audio_capture_start(struct wd_audio_capture* capture) {
    (void)capture;
    return false;
}

void wd_audio_capture_stop(struct wd_audio_capture* capture) {
    (void)capture;
}

bool wd_audio_capture_available(void) {
    return false;
}

const char* wd_audio_capture_backend_name(void) {
    return "unavailable";
}

const char* wd_audio_capture_sink_name(const struct wd_audio_capture* capture) {
    (void)capture;
    return NULL;
}

const char* wd_audio_capture_sink_target(const struct wd_audio_capture* capture) {
    (void)capture;
    return NULL;
}

#endif
