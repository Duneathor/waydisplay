#include "wd_audio_capture.h"

#include "waydisplay/wd_log.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_time.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE
#define WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE 0
#endif

#if WAYDISPLAY_HAVE_PIPEWIRE_AUDIO_CAPTURE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/defs.h>

struct wd_audio_capture {
    struct pw_thread_loop*    loop;
    struct pw_stream*         stream;
    wd_audio_capture_callback callback;
    void*                     userdata;
    uint8_t                   channels;
    bool                      ready;
    bool                      failed;
    bool                      delivery_enabled;
    bool                      shutting_down;
    uint64_t                  delivery_generation;
    uint32_t                  active_callbacks;
    char                      sink_name[96];
    char                      sink_target[96];
};

static void wd_audio_capture_process(void* userdata) {
    struct wd_audio_capture* capture = userdata;
    if (!capture || !capture->stream)
    {
        return;
    }

    struct pw_buffer* pw_buffer = pw_stream_dequeue_buffer(capture->stream);
    if (!pw_buffer)
    {
        return;
    }

    const uint64_t generation = __atomic_load_n(&capture->delivery_generation, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&capture->active_callbacks, 1, __ATOMIC_ACQ_REL);

    struct spa_buffer*      buffer     = pw_buffer->buffer;
    struct spa_data*        data       = buffer && buffer->n_datas > 0 ? &buffer->datas[0] : NULL;
    const struct spa_chunk* chunk      = data ? data->chunk : NULL;
    const uint32_t          frame_size = (uint32_t)capture->channels * sizeof(float);
    const float*            samples    = NULL;
    uint32_t                frames     = 0;

    if (data && data->data && chunk && frame_size != 0)
    {
        const uint32_t offset = SPA_MIN(chunk->offset, data->maxsize);
        const uint32_t bytes  = SPA_MIN(chunk->size, data->maxsize - offset);
        samples               = SPA_PTROFF(data->data, offset, const float);
        frames                = bytes / frame_size;
    }

    if (samples && frames > 0 && generation == __atomic_load_n(&capture->delivery_generation, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&capture->delivery_enabled, __ATOMIC_ACQUIRE))
    {
        struct pw_time                       time      = {0};
        const bool                           have_time = pw_stream_get_time_n(capture->stream, &time, sizeof(time)) >= 0;
        const struct wd_audio_capture_timing timing    = {
            .cycle_start_ns    = have_time && time.now > 0 ? (uint64_t)time.now : wd_now_ns(),
            .position          = have_time ? time.ticks : 0,
            .clock_id          = 0,
            .rate_num          = have_time ? time.rate.num : 0,
            .rate_denom        = have_time ? time.rate.denom : 0,
            .position_reliable = have_time && time.rate.num != 0 && time.rate.denom != 0,
            .discontinuity     = false,
        };
        capture->callback(capture->userdata, samples, frames, capture->channels, &timing);
    }

    __atomic_sub_fetch(&capture->active_callbacks, 1, __ATOMIC_ACQ_REL);
    pw_stream_queue_buffer(capture->stream, pw_buffer);
}

static void wd_audio_capture_state_changed(void* userdata, enum pw_stream_state old_state, enum pw_stream_state state, const char* error) {
    struct wd_audio_capture* capture = userdata;
    if (!capture)
    {
        return;
    }
    const bool shutting_down = __atomic_load_n(&capture->shutting_down, __ATOMIC_ACQUIRE);
    if (!shutting_down && (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING))
    {
        __atomic_store_n(&capture->failed, false, __ATOMIC_RELEASE);
        __atomic_store_n(&capture->ready, true, __ATOMIC_RELEASE);
    }
    else
    {
        __atomic_store_n(&capture->ready, false, __ATOMIC_RELEASE);
        if (!shutting_down &&
            (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old_state != PW_STREAM_STATE_UNCONNECTED)))
        {
            __atomic_store_n(&capture->failed, true, __ATOMIC_RELEASE);
            __atomic_store_n(&capture->delivery_enabled, false, __ATOMIC_RELEASE);
            __atomic_add_fetch(&capture->delivery_generation, 1, __ATOMIC_ACQ_REL);
        }
        if (!shutting_down && state == PW_STREAM_STATE_ERROR)
        {
            WD_LOG_ERROR("PipeWire private audio sink error: %s", error ? error : "unknown");
        }
        else if (!shutting_down && state == PW_STREAM_STATE_UNCONNECTED && old_state != PW_STREAM_STATE_UNCONNECTED)
        {
            WD_LOG_WARN("PipeWire private audio sink disconnected");
        }
    }
    pw_thread_loop_signal(capture->loop, false);
}

static const struct pw_stream_events wd_audio_capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = wd_audio_capture_state_changed,
    .process       = wd_audio_capture_process,
};

static void wd_audio_capture_cleanup(struct wd_audio_capture* capture) {
    if (!capture)
    {
        return;
    }
    if (capture->loop)
    {
        __atomic_store_n(&capture->shutting_down, true, __ATOMIC_RELEASE);
        __atomic_store_n(&capture->ready, false, __ATOMIC_RELEASE);
        pw_thread_loop_lock(capture->loop);
        if (capture->stream)
        {
            pw_stream_destroy(capture->stream);
            capture->stream = NULL;
        }
        pw_thread_loop_unlock(capture->loop);
        pw_thread_loop_stop(capture->loop);
        pw_thread_loop_destroy(capture->loop);
        capture->loop = NULL;
    }
}

bool wd_audio_capture_create(struct wd_audio_capture** out_capture, uint8_t channels, wd_audio_capture_callback callback, void* userdata) {
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
    snprintf(capture->sink_name, sizeof(capture->sink_name), "waydisplay.audio.sink.%ld", (long)getpid());
    snprintf(capture->sink_target, sizeof(capture->sink_target), "%s", capture->sink_name);

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
    char owner_scope[64];
    snprintf(channel_count, sizeof(channel_count), "%u", channels);
    snprintf(owner_scope, sizeof(owner_scope), "waydisplay.%ld", (long)getpid());
    capture->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(capture->loop), "WayDisplay private audio sink",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_MEDIA_CLASS,
                          "Audio/Sink", PW_KEY_NODE_NAME, capture->sink_name, PW_KEY_NODE_DESCRIPTION, "WayDisplay private audio sink",
                          PW_KEY_NODE_VIRTUAL, "true", "node.terminal", "true", "node.pause-on-idle", "true", "priority.session", "0",
                          "state.restore-props", "false", "waydisplay.audio.private", "true", "waydisplay.audio.scope", owner_scope,
                          PW_KEY_AUDIO_FORMAT, "F32", PW_KEY_AUDIO_RATE, "48000", PW_KEY_NODE_RATE, "1/48000", PW_KEY_AUDIO_CHANNELS,
                          channel_count, SPA_KEY_AUDIO_POSITION, channels == 1 ? "[ MONO ]" : "[ FL FR ]", NULL),
        &wd_audio_capture_events, capture);
    if (!capture->stream)
    {
        pw_thread_loop_unlock(capture->loop);
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    struct spa_audio_info_raw audio_info = {0};
    audio_info.format                    = SPA_AUDIO_FORMAT_F32;
    audio_info.rate                      = WD_AUDIO_SAMPLE_RATE_DEFAULT;
    audio_info.channels                  = channels;
    audio_info.position[0]               = channels == 1 ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_FL;
    if (channels > 1)
    {
        audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    uint8_t                params_buffer[1024];
    struct spa_pod_builder builder   = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod*  params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info),
    };
    if (pw_stream_connect(capture->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0)
    {
        pw_thread_loop_unlock(capture->loop);
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    while (!__atomic_load_n(&capture->ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&capture->failed, __ATOMIC_ACQUIRE))
    {
        const int wait_result = pw_thread_loop_timed_wait(capture->loop, 3);
        if (wait_result != 0)
        {
            WD_LOG_ERROR("timed out waiting for PipeWire private audio sink");
            __atomic_store_n(&capture->failed, true, __ATOMIC_RELEASE);
            break;
        }
    }
    pw_thread_loop_unlock(capture->loop);

    if (!wd_audio_capture_healthy(capture))
    {
        wd_audio_capture_cleanup(capture);
        free(capture);
        return false;
    }

    WD_LOG_DEBUG("private audio sink ready: node=%s target=%s rate=%u channels=%u", capture->sink_name, capture->sink_target,
                 WD_AUDIO_SAMPLE_RATE_DEFAULT, channels);
    *out_capture = capture;
    return true;
}

void wd_audio_capture_destroy(struct wd_audio_capture* capture) {
    if (!capture)
    {
        return;
    }
    wd_audio_capture_stop(capture);
    wd_audio_capture_cleanup(capture);
    free(capture);
}

bool wd_audio_capture_start(struct wd_audio_capture* capture) {
    if (!wd_audio_capture_healthy(capture))
    {
        return false;
    }
    /* Change the generation before enabling delivery. A callback that began
     * during the preceding stopped epoch will observe a mismatch and return,
     * even if it resumes after this activation becomes visible. */
    __atomic_add_fetch(&capture->delivery_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&capture->delivery_enabled, true, __ATOMIC_RELEASE);
    if (!wd_audio_capture_healthy(capture))
    {
        __atomic_store_n(&capture->delivery_enabled, false, __ATOMIC_RELEASE);
        __atomic_add_fetch(&capture->delivery_generation, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

void wd_audio_capture_stop(struct wd_audio_capture* capture) {
    if (!capture)
    {
        return;
    }

    /* Disable first, then advance the generation. Callbacks that have not yet
     * registered themselves will fail either the enabled check or the second
     * generation check. Callbacks that already passed both checks are counted
     * and drained before this stop completes. */
    __atomic_store_n(&capture->delivery_enabled, false, __ATOMIC_RELEASE);
    __atomic_add_fetch(&capture->delivery_generation, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&capture->active_callbacks, __ATOMIC_ACQUIRE) != 0)
    {
        sched_yield();
    }
}

bool wd_audio_capture_available(void) {
    return true;
}

bool wd_audio_capture_healthy(const struct wd_audio_capture* capture) {
    return capture && !__atomic_load_n(&capture->shutting_down, __ATOMIC_ACQUIRE) && __atomic_load_n(&capture->ready, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&capture->failed, __ATOMIC_ACQUIRE);
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

bool wd_audio_capture_create(struct wd_audio_capture** out_capture, uint8_t channels, wd_audio_capture_callback callback, void* userdata) {
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

bool wd_audio_capture_healthy(const struct wd_audio_capture* capture) {
    (void)capture;
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
