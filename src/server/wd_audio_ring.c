#include "wd_audio_ring.h"

#include <stdlib.h>
#include <string.h>

static bool wd_audio_size_multiply(size_t a, size_t b, size_t* out) {
    if (!out || (a != 0 && b > SIZE_MAX / a))
    {
        return false;
    }
    *out = a * b;
    return true;
}

bool wd_audio_pcm_ring_init(struct wd_audio_pcm_ring* ring, uint32_t capacity_frames, uint8_t channels) {
    if (!ring || capacity_frames == 0 || channels == 0 || channels > 2)
    {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    size_t sample_count = 0;
    size_t sample_bytes = 0;
    size_t pts_bytes    = 0;
    if (!wd_audio_size_multiply((size_t)capacity_frames, (size_t)channels, &sample_count) ||
        !wd_audio_size_multiply(sample_count, sizeof(*ring->samples), &sample_bytes) ||
        !wd_audio_size_multiply((size_t)capacity_frames, sizeof(*ring->pts_samples), &pts_bytes))
    {
        return false;
    }
    ring->samples     = calloc(1, sample_bytes);
    ring->pts_samples = calloc(1, pts_bytes);
    if (!ring->samples || !ring->pts_samples)
    {
        wd_audio_pcm_ring_finish(ring);
        return false;
    }
    ring->capacity_frames = capacity_frames;
    ring->channels        = channels;
    __atomic_store_n(&ring->read_frame, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->write_frame, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->overruns, 0, __ATOMIC_RELAXED);
    return true;
}

void wd_audio_pcm_ring_finish(struct wd_audio_pcm_ring* ring) {
    if (!ring)
    {
        return;
    }
    free(ring->samples);
    free(ring->pts_samples);
    memset(ring, 0, sizeof(*ring));
}

void wd_audio_pcm_ring_reset(struct wd_audio_pcm_ring* ring) {
    if (!ring)
    {
        return;
    }
    __atomic_store_n(&ring->read_frame, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->write_frame, 0, __ATOMIC_RELEASE);
}

void wd_audio_pcm_ring_drop_all(struct wd_audio_pcm_ring* ring) {
    if (!ring)
    {
        return;
    }
    const uint64_t write = __atomic_load_n(&ring->write_frame, __ATOMIC_ACQUIRE);
    __atomic_store_n(&ring->read_frame, write, __ATOMIC_RELEASE);
}

bool wd_audio_pcm_ring_write(struct wd_audio_pcm_ring* ring, const float* samples, uint32_t frames, uint64_t first_pts_sample) {
    if (!ring || !ring->samples || !ring->pts_samples || !samples || frames == 0 || frames > ring->capacity_frames)
    {
        return false;
    }

    const uint64_t write = __atomic_load_n(&ring->write_frame, __ATOMIC_RELAXED);
    const uint64_t read  = __atomic_load_n(&ring->read_frame, __ATOMIC_ACQUIRE);
    if (write - read > ring->capacity_frames || frames > ring->capacity_frames - (uint32_t)(write - read))
    {
        __atomic_fetch_add(&ring->overruns, 1, __ATOMIC_RELAXED);
        return false;
    }

    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const uint32_t slot = (uint32_t)((write + frame) % ring->capacity_frames);
        memcpy(&ring->samples[(size_t)slot * ring->channels], &samples[(size_t)frame * ring->channels],
               (size_t)ring->channels * sizeof(float));
        ring->pts_samples[slot] = first_pts_sample + frame;
    }
    __atomic_store_n(&ring->write_frame, write + frames, __ATOMIC_RELEASE);
    return true;
}

bool wd_audio_pcm_ring_write_capture(struct wd_audio_pcm_ring* ring, const float* samples, uint32_t frames, uint8_t source_channels,
                                     bool silent, uint64_t first_pts_sample) {
    if (!ring || !ring->samples || !ring->pts_samples || frames == 0 || frames > ring->capacity_frames || source_channels == 0 ||
        source_channels > 2 || (!silent && !samples))
    {
        return false;
    }
    if (!silent && source_channels != ring->channels && !(source_channels == 2 && ring->channels == 1))
    {
        return false;
    }

    const uint64_t write = __atomic_load_n(&ring->write_frame, __ATOMIC_RELAXED);
    const uint64_t read  = __atomic_load_n(&ring->read_frame, __ATOMIC_ACQUIRE);
    if (write - read > ring->capacity_frames || frames > ring->capacity_frames - (uint32_t)(write - read))
    {
        __atomic_fetch_add(&ring->overruns, 1, __ATOMIC_RELAXED);
        return false;
    }

    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const uint32_t slot = (uint32_t)((write + frame) % ring->capacity_frames);
        float* destination = &ring->samples[(size_t)slot * ring->channels];
        if (silent)
        {
            for (uint8_t channel = 0; channel < ring->channels; ++channel)
            {
                destination[channel] = 0.0f;
            }
        }
        else if (source_channels == ring->channels)
        {
            memcpy(destination, &samples[(size_t)frame * source_channels], (size_t)ring->channels * sizeof(float));
        }
        else
        {
            const float left  = samples[(size_t)frame * 2u];
            const float right = samples[(size_t)frame * 2u + 1u];
            destination[0] = (left + right) * 0.5f;
        }
        ring->pts_samples[slot] = first_pts_sample + frame;
    }
    __atomic_store_n(&ring->write_frame, write + frames, __ATOMIC_RELEASE);
    return true;
}

bool wd_audio_pcm_ring_write_planar(struct wd_audio_pcm_ring* ring, const float* const* planes, uint32_t frames,
                                    uint64_t first_pts_sample) {
    if (!ring || !ring->samples || !ring->pts_samples || !planes || frames == 0 || frames > ring->capacity_frames)
    {
        return false;
    }
    for (uint8_t channel = 0; channel < ring->channels; ++channel)
    {
        if (!planes[channel])
        {
            return false;
        }
    }

    const uint64_t write = __atomic_load_n(&ring->write_frame, __ATOMIC_RELAXED);
    const uint64_t read  = __atomic_load_n(&ring->read_frame, __ATOMIC_ACQUIRE);
    if (write - read > ring->capacity_frames || frames > ring->capacity_frames - (uint32_t)(write - read))
    {
        __atomic_fetch_add(&ring->overruns, 1, __ATOMIC_RELAXED);
        return false;
    }

    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const uint32_t slot = (uint32_t)((write + frame) % ring->capacity_frames);
        for (uint8_t channel = 0; channel < ring->channels; ++channel)
        {
            ring->samples[(size_t)slot * ring->channels + channel] = planes[channel][frame];
        }
        ring->pts_samples[slot] = first_pts_sample + frame;
    }
    __atomic_store_n(&ring->write_frame, write + frames, __ATOMIC_RELEASE);
    return true;
}

uint32_t wd_audio_pcm_ring_read(struct wd_audio_pcm_ring* ring, float* samples, uint32_t frames, uint64_t* first_pts_sample,
                                bool* continuity) {
    if (!ring || !ring->samples || !ring->pts_samples || !samples || frames == 0)
    {
        return 0;
    }
    const uint64_t read  = __atomic_load_n(&ring->read_frame, __ATOMIC_RELAXED);
    const uint64_t write = __atomic_load_n(&ring->write_frame, __ATOMIC_ACQUIRE);
    if (write - read < frames)
    {
        return 0;
    }

    bool     contiguous = true;
    uint64_t first      = 0;
    uint64_t previous   = 0;
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const uint32_t slot = (uint32_t)((read + frame) % ring->capacity_frames);
        const uint64_t pts  = ring->pts_samples[slot];
        if (frame == 0)
        {
            first = pts;
        }
        else if (pts != previous + 1)
        {
            contiguous = false;
        }
        previous = pts;
        memcpy(&samples[(size_t)frame * ring->channels], &ring->samples[(size_t)slot * ring->channels],
               (size_t)ring->channels * sizeof(float));
    }
    __atomic_store_n(&ring->read_frame, read + frames, __ATOMIC_RELEASE);
    if (first_pts_sample)
    {
        *first_pts_sample = first;
    }
    if (continuity)
    {
        *continuity = contiguous;
    }
    return frames;
}

uint32_t wd_audio_pcm_ring_queued_frames(const struct wd_audio_pcm_ring* ring) {
    if (!ring)
    {
        return 0;
    }
    const uint64_t read   = __atomic_load_n(&ring->read_frame, __ATOMIC_ACQUIRE);
    const uint64_t write  = __atomic_load_n(&ring->write_frame, __ATOMIC_ACQUIRE);
    const uint64_t queued = write >= read ? write - read : 0;
    return queued > UINT32_MAX ? UINT32_MAX : (uint32_t)queued;
}

uint64_t wd_audio_pcm_ring_overruns(const struct wd_audio_pcm_ring* ring) {
    return ring ? __atomic_load_n(&ring->overruns, __ATOMIC_RELAXED) : 0;
}
