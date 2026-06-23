#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_audio_stream;

struct wd_audio_stream_stats {
    uint64_t captured_frames;
    uint64_t capture_overruns;
    uint64_t encoded_packets;
    uint64_t encoded_bytes;
    uint64_t queue_drops;
    uint64_t discontinuities;
    uint64_t encode_failures;
};

bool wd_audio_stream_create(struct wd_audio_stream** out_stream);
void wd_audio_stream_destroy(struct wd_audio_stream* stream);

bool        wd_audio_stream_available(void);
bool        wd_audio_stream_ready(struct wd_audio_stream* stream);
const char* wd_audio_stream_sink_name(const struct wd_audio_stream* stream);
const char* wd_audio_stream_sink_target(const struct wd_audio_stream* stream);
const char* wd_audio_stream_capture_backend_name(void);
const char* wd_audio_stream_encoder_backend_name(void);

bool wd_audio_stream_start(struct wd_audio_stream* stream, int tcp_fd, uint8_t session_id, uint64_t connection_token, uint64_t audio_epoch,
                           uint64_t media_clock_id, uint64_t media_clock_start_ns, uint8_t channels, uint32_t bitrate,
                           uint16_t target_latency_ms);
void wd_audio_stream_stop(struct wd_audio_stream* stream);
bool wd_audio_stream_running(const struct wd_audio_stream* stream);
void wd_audio_stream_get_stats(const struct wd_audio_stream* stream, struct wd_audio_stream_stats* stats);

#ifdef __cplusplus
}
#endif
