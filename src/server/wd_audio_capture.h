#pragma once

#include <stdbool.h>
#include <stdint.h>

struct wd_audio_capture;

struct wd_audio_capture_timing {
    uint64_t cycle_start_ns;
    uint64_t position;
    uint32_t clock_id;
    uint32_t rate_num;
    uint32_t rate_denom;
    bool position_reliable;
    bool discontinuity;
};

typedef void (*wd_audio_capture_callback)(
    void* userdata, const float* samples, uint32_t frames, uint8_t channels,
    const struct wd_audio_capture_timing* timing);

/* Create and connect a private PipeWire sink. The sink is persistent so the
 * launched application's target remains stable across remote reconnects. */
bool wd_audio_capture_create(struct wd_audio_capture** out_capture, uint8_t channels,
                             wd_audio_capture_callback callback, void* userdata);
void wd_audio_capture_destroy(struct wd_audio_capture* capture);

/* Enable or disable delivery to callback. The PipeWire sink itself remains
 * connected and consumes audio while delivery is disabled. */
bool wd_audio_capture_start(struct wd_audio_capture* capture);
void wd_audio_capture_stop(struct wd_audio_capture* capture);

bool wd_audio_capture_available(void);
bool wd_audio_capture_healthy(const struct wd_audio_capture* capture);
const char* wd_audio_capture_backend_name(void);
const char* wd_audio_capture_sink_name(const struct wd_audio_capture* capture);
const char* wd_audio_capture_sink_target(const struct wd_audio_capture* capture);
