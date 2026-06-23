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

inline uint64_t client_audio_device_playhead(uint64_t start_pts, uint64_t submitted_end_pts, uint64_t mixed_samples_fp,
                                             uint64_t device_buffer_samples) {
    const uint64_t mixed_samples     = mixed_samples_fp >> CLIENT_AUDIO_CLOCK_FRACTION_BITS;
    const uint64_t presented_samples = mixed_samples > device_buffer_samples ? mixed_samples - device_buffer_samples : 0;
    const uint64_t available_samples = submitted_end_pts >= start_pts ? submitted_end_pts - start_pts : 0;
    return start_pts + std::min(presented_samples, available_samples);
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
