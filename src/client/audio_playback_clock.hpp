#pragma once

#include <algorithm>
#include <cstdint>

namespace waydisplay {

constexpr uint64_t CLIENT_AUDIO_CLOCK_FRACTION_BITS = 32u;
constexpr uint64_t CLIENT_AUDIO_CLOCK_FRACTION_ONE  = uint64_t{1} << CLIENT_AUDIO_CLOCK_FRACTION_BITS;

inline uint64_t client_audio_frames_to_samples_fp(uint64_t frames, uint32_t frame_rate, uint32_t sample_rate) {
    if (frame_rate == 0 || sample_rate == 0)
    {
        return 0;
    }

    const uint64_t whole_frames     = frames / frame_rate;
    const uint64_t remainder_frames = frames % frame_rate;
    if (whole_frames > UINT64_MAX / sample_rate)
    {
        return UINT64_MAX;
    }

    uint64_t       whole_samples           = whole_frames * sample_rate;
    const uint64_t remainder_product       = remainder_frames * sample_rate;
    const uint64_t remainder_whole_samples = remainder_product / frame_rate;
    const uint64_t fractional_numerator    = remainder_product % frame_rate;
    if (UINT64_MAX - whole_samples < remainder_whole_samples)
    {
        return UINT64_MAX;
    }
    whole_samples += remainder_whole_samples;
    if (whole_samples > (UINT64_MAX >> CLIENT_AUDIO_CLOCK_FRACTION_BITS))
    {
        return UINT64_MAX;
    }

    const uint64_t fractional_samples_fp = (fractional_numerator * CLIENT_AUDIO_CLOCK_FRACTION_ONE) / frame_rate;
    return whole_samples * CLIENT_AUDIO_CLOCK_FRACTION_ONE + fractional_samples_fp;
}

/* The target latency is a startup threshold, not a lifetime bound.  When
 * the SDL input FIFO grows, audio's presentation clock trails the live video
 * timeline indefinitely.  Rebase at a generous multiple of the startup
 * target, with room for device callbacks and packet-delivery jitter. */
inline uint64_t client_audio_max_queued_samples(uint32_t sample_rate, uint16_t target_latency_ms) {
    const uint64_t max_ms = std::max<uint64_t>(120, static_cast<uint64_t>(target_latency_ms) * 4u);
    return (static_cast<uint64_t>(sample_rate) * max_ms) / 1000u;
}

inline bool client_audio_output_rebase_needed(bool playing, uint64_t queued_samples, uint64_t incoming_samples,
                                               uint64_t max_queued_samples) {
    return playing && max_queued_samples != 0 && incoming_samples != 0 &&
           (queued_samples > max_queued_samples || incoming_samples > max_queued_samples - queued_samples);
}

inline uint64_t client_audio_device_playhead(uint64_t start_pts, uint64_t submitted_end_pts, uint64_t mixed_samples_fp,
                                             uint64_t device_buffer_samples) {
    const uint64_t mixed_samples     = mixed_samples_fp >> CLIENT_AUDIO_CLOCK_FRACTION_BITS;
    const uint64_t presented_samples = mixed_samples > device_buffer_samples ? mixed_samples - device_buffer_samples : 0;
    const uint64_t available_samples = submitted_end_pts >= start_pts ? submitted_end_pts - start_pts : 0;
    return start_pts + std::min(presented_samples, available_samples);
}

/* Postmix counts the entire device mix, including silence. It cannot prove
 * that this stream's still-queued PCM was mixed. Bound its playhead by both
 * the callback estimate and this stream's queued media timeline. */
inline uint64_t client_audio_device_playhead_queued(uint64_t start_pts, uint64_t submitted_end_pts,
                                                     uint64_t mixed_samples_fp, uint64_t device_buffer_samples,
                                                     uint64_t stream_queued_samples) {
    const uint64_t callback_playhead =
        client_audio_device_playhead(start_pts, submitted_end_pts, mixed_samples_fp, device_buffer_samples);
    const uint64_t available = submitted_end_pts >= start_pts ? submitted_end_pts - start_pts : 0;
    const uint64_t queued = std::min(available, stream_queued_samples);
    const uint64_t mixed_limit = available - queued;
    const uint64_t played_limit = mixed_limit > device_buffer_samples ? mixed_limit - device_buffer_samples : 0;
    return std::min(callback_playhead, start_pts + played_limit);
}

inline bool client_audio_device_consumed(uint64_t start_pts, uint64_t submitted_end_pts, uint64_t mixed_samples_fp,
                                         uint64_t device_buffer_samples) {
    if (submitted_end_pts <= start_pts)
    {
        return false;
    }
    const uint64_t mixed_samples     = mixed_samples_fp >> CLIENT_AUDIO_CLOCK_FRACTION_BITS;
    const uint64_t presented_samples = mixed_samples > device_buffer_samples ? mixed_samples - device_buffer_samples : 0;
    return presented_samples >= submitted_end_pts - start_pts;
}

} // namespace waydisplay
