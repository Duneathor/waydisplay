#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_audio_pcm_ring {
    float* samples;
    uint64_t* pts_samples;
    uint32_t capacity_frames;
    uint8_t channels;
    uint64_t read_frame;
    uint64_t write_frame;
    uint64_t overruns;
};

bool wd_audio_pcm_ring_init(struct wd_audio_pcm_ring* ring, uint32_t capacity_frames,
                            uint8_t channels);
void wd_audio_pcm_ring_finish(struct wd_audio_pcm_ring* ring);
void wd_audio_pcm_ring_reset(struct wd_audio_pcm_ring* ring);
/* Single-consumer operation that discards all currently queued frames without
 * disturbing frames concurrently appended after the observed write cursor. */
void wd_audio_pcm_ring_drop_all(struct wd_audio_pcm_ring* ring);

/* Single producer. Returns false and drops the whole block when bounded capacity
 * would be exceeded. This operation performs no allocation and takes no locks. */
bool wd_audio_pcm_ring_write(struct wd_audio_pcm_ring* ring, const float* samples,
                             uint32_t frames, uint64_t first_pts_sample);

/* Single-producer planar variant used by PipeWire DSP ports. */
bool wd_audio_pcm_ring_write_planar(struct wd_audio_pcm_ring* ring,
                                    const float* const* planes,
                                    uint32_t frames, uint64_t first_pts_sample);

/* Single consumer. Returns exactly frames or zero. continuity is false when the
 * sample timestamps reveal a capture discontinuity. */
uint32_t wd_audio_pcm_ring_read(struct wd_audio_pcm_ring* ring, float* samples,
                                uint32_t frames, uint64_t* first_pts_sample,
                                bool* continuity);

uint32_t wd_audio_pcm_ring_queued_frames(const struct wd_audio_pcm_ring* ring);
uint64_t wd_audio_pcm_ring_overruns(const struct wd_audio_pcm_ring* ring);

#ifdef __cplusplus
}
#endif
