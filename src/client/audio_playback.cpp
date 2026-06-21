#include "audio_playback.hpp"

#include "waydisplay/wd_log.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#if WAYDISPLAY_HAVE_OPUS_AUDIO
#include <opus/opus.h>
#endif

namespace waydisplay {

struct ClientAudioPlayback {
    std::mutex mutex;
    SDL_AudioStream* stream = nullptr;
#if WAYDISPLAY_HAVE_OPUS_AUDIO
    OpusDecoder* decoder = nullptr;
#endif
    wd_audio_config_payload config{};
    bool configured = false;
    bool playing = false;
    uint64_t expected_sequence = 0;
    uint64_t expected_pts_samples = 0;
    bool have_expected_pts = false;
    uint64_t submitted_end_pts = 0;
    uint16_t target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;
    uint16_t pre_skip_remaining = 0;
    uint64_t underflows = 0;
    uint64_t late_drops = 0;
    uint64_t discontinuities = 0;
    std::vector<float> decode_buffer;
};

namespace {

void destroy_stream_locked(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    if (playback->stream)
    {
        SDL_DestroyAudioStream(playback->stream);
        playback->stream = nullptr;
    }
#if WAYDISPLAY_HAVE_OPUS_AUDIO
    if (playback->decoder)
    {
        opus_decoder_destroy(playback->decoder);
        playback->decoder = nullptr;
    }
#endif
    playback->configured = false;
    playback->playing = false;
    playback->expected_sequence = 0;
    playback->expected_pts_samples = 0;
    playback->have_expected_pts = false;
    playback->submitted_end_pts = 0;
    playback->target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;
    playback->pre_skip_remaining = 0;
    playback->decode_buffer.clear();
}

bool reset_decoder_locked(ClientAudioPlayback* playback) {
#if WAYDISPLAY_HAVE_OPUS_AUDIO
    if (!playback || !playback->decoder)
    {
        return false;
    }
    return opus_decoder_ctl(playback->decoder, OPUS_RESET_STATE) == OPUS_OK;
#else
    (void)playback;
    return false;
#endif
}

uint64_t queued_samples_locked(ClientAudioPlayback* playback) {
    if (!playback || !playback->stream || playback->config.channels == 0)
    {
        return 0;
    }
    const int queued_bytes = SDL_GetAudioStreamQueued(playback->stream);
    if (queued_bytes <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(queued_bytes) /
           (sizeof(float) * playback->config.channels);
}

void clear_for_discontinuity_locked(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    if (playback->stream)
    {
        SDL_ClearAudioStream(playback->stream);
        SDL_PauseAudioStreamDevice(playback->stream);
    }
    (void)reset_decoder_locked(playback);
    playback->playing = false;
    playback->submitted_end_pts = 0;
    playback->expected_pts_samples = 0;
    playback->have_expected_pts = false;
    playback->pre_skip_remaining = playback->config.codec_delay_samples;
    playback->discontinuities++;
}

} // namespace

bool client_audio_playback_create(ClientAudioPlayback** out_playback) {
    if (!out_playback)
    {
        return false;
    }
    *out_playback = new (std::nothrow) ClientAudioPlayback();
    return *out_playback != nullptr;
}

void client_audio_playback_destroy(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(playback->mutex);
        destroy_stream_locked(playback);
    }
    delete playback;
}

bool client_audio_playback_available() {
#if WAYDISPLAY_HAVE_OPUS_AUDIO
    return true;
#else
    return false;
#endif
}

const char* client_audio_playback_backend_name() {
#if WAYDISPLAY_HAVE_OPUS_AUDIO
    return "opus/sdl3";
#else
    return "unavailable";
#endif
}

bool client_audio_playback_configure(ClientAudioPlayback* playback,
                                     const wd_audio_config_payload& config,
                                     uint16_t target_latency_ms) {
    if (!playback || !wd_audio_config_payload_is_valid(&config, sizeof(config)) ||
        target_latency_ms < WD_AUDIO_TARGET_LATENCY_MS_MIN ||
        target_latency_ms > WD_AUDIO_TARGET_LATENCY_MS_MAX)
    {
        return false;
    }
#if !WAYDISPLAY_HAVE_OPUS_AUDIO
    (void)config;
    return false;
#else
    std::lock_guard<std::mutex> lock(playback->mutex);
    destroy_stream_locked(playback);

    int error = OPUS_OK;
    playback->decoder = opus_decoder_create(static_cast<opus_int32>(config.sample_rate),
                                             config.channels, &error);
    if (!playback->decoder || error != OPUS_OK)
    {
        destroy_stream_locked(playback);
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = config.channels;
    spec.freq = static_cast<int>(config.sample_rate);
    playback->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                  &spec, nullptr, nullptr);
    if (!playback->stream)
    {
        WD_LOG_ERROR("failed to open SDL audio playback: %s", SDL_GetError());
        destroy_stream_locked(playback);
        return false;
    }

    playback->config = config;
    playback->target_latency_ms = target_latency_ms;
    playback->pre_skip_remaining = config.codec_delay_samples;
    playback->configured = true;
    playback->decode_buffer.resize(static_cast<size_t>(5760u) * config.channels);
    WD_LOG_INFO("audio playback configured: codec=opus rate=%u channels=%u frame_samples=%u bitrate=%u",
                config.sample_rate, config.channels, config.frame_samples,
                config.target_bitrate);
    return true;
#endif
}

bool client_audio_playback_handle_packet(ClientAudioPlayback* playback,
                                         const uint8_t* payload,
                                         uint32_t payload_size) {
    if (!playback || !payload || payload_size < sizeof(wd_audio_packet_payload_header))
    {
        return false;
    }

    wd_audio_packet_payload_header header{};
    std::memcpy(&header, payload, sizeof(header));
    if (!wd_audio_packet_payload_size_is_valid(&header, payload_size))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(playback->mutex);
    if (!playback->configured || header.session_id != playback->config.session_id ||
        header.connection_token != playback->config.connection_token ||
        header.audio_epoch != playback->config.audio_epoch ||
        header.media_clock_id != playback->config.media_clock_id)
    {
        return false;
    }

    bool discontinuity = false;
    if (playback->expected_sequence != 0 && header.sequence != playback->expected_sequence)
    {
        discontinuity = true;
    }
    if ((header.flags & WD_AUDIO_PACKET_DISCONTINUITY) != 0)
    {
        discontinuity = true;
    }
    if (discontinuity)
    {
        clear_for_discontinuity_locked(playback);
    }
    else if (playback->have_expected_pts && header.pts_samples != playback->expected_pts_samples)
    {
        clear_for_discontinuity_locked(playback);
        return false;
    }
    playback->expected_sequence = header.sequence + 1;

    if ((header.flags & WD_AUDIO_PACKET_END_OF_STREAM) != 0)
    {
        if (playback->stream)
        {
            SDL_PauseAudioStreamDevice(playback->stream);
        }
        playback->playing = false;
        return true;
    }

#if !WAYDISPLAY_HAVE_OPUS_AUDIO
    return false;
#else
    if (header.duration_samples != playback->config.frame_samples)
    {
        return false;
    }
    const uint64_t queued_before = queued_samples_locked(playback);
    if (playback->playing && queued_before == 0)
    {
        playback->underflows++;
        playback->playing = false;
        SDL_PauseAudioStreamDevice(playback->stream);
    }
    const uint64_t current_playhead = playback->submitted_end_pts >= queued_before
                                          ? playback->submitted_end_pts - queued_before
                                          : 0;
    const uint64_t late_limit = (WD_AUDIO_SAMPLE_RATE_DEFAULT * 120u) / 1000u;
    if (playback->playing && header.pts_samples + header.duration_samples + late_limit < current_playhead)
    {
        playback->late_drops++;
        return true;
    }

    const uint8_t* packet = payload + sizeof(header);
    const int decoded = opus_decode_float(playback->decoder, packet,
                                          static_cast<opus_int32>(header.data_size),
                                          playback->decode_buffer.data(), 5760, 0);
    if (decoded <= 0 || decoded != header.duration_samples)
    {
        clear_for_discontinuity_locked(playback);
        return false;
    }
    const uint16_t skip = static_cast<uint16_t>(std::min<int>(
        decoded, playback->pre_skip_remaining));
    playback->pre_skip_remaining = static_cast<uint16_t>(
        playback->pre_skip_remaining - skip);
    const int queued_frames = decoded - skip;
    if (queued_frames > 0)
    {
        const float* queued_pcm = playback->decode_buffer.data() +
                                  static_cast<size_t>(skip) * playback->config.channels;
        const int byte_count = queued_frames * playback->config.channels *
                               static_cast<int>(sizeof(float));
        if (!SDL_PutAudioStreamData(playback->stream, queued_pcm, byte_count))
        {
            WD_LOG_ERROR("failed to queue SDL audio: %s", SDL_GetError());
            clear_for_discontinuity_locked(playback);
            return false;
        }
    }
    playback->submitted_end_pts = header.pts_samples + static_cast<uint64_t>(decoded);
    playback->expected_pts_samples = playback->submitted_end_pts;
    playback->have_expected_pts = true;

    const uint64_t target_samples =
        (static_cast<uint64_t>(playback->config.sample_rate) *
         playback->target_latency_ms) / 1000u;
    if (!playback->playing && queued_samples_locked(playback) >= target_samples)
    {
        if (SDL_ResumeAudioStreamDevice(playback->stream))
        {
            playback->playing = true;
        }
    }
    return true;
#endif
}

void client_audio_playback_reset(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(playback->mutex);
    destroy_stream_locked(playback);
}

bool client_audio_playback_is_configured(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(playback->mutex);
    return playback->configured;
}

bool client_audio_playback_is_playing(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(playback->mutex);
    return playback->playing;
}

bool client_audio_playback_playhead_samples(ClientAudioPlayback* playback,
                                            uint64_t* playhead_samples) {
    if (!playback || !playhead_samples)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(playback->mutex);
    if (!playback->configured || !playback->playing)
    {
        return false;
    }
    const uint64_t queued = queued_samples_locked(playback);
    *playhead_samples = playback->submitted_end_pts >= queued
                            ? playback->submitted_end_pts - queued
                            : 0;
    return true;
}

uint64_t client_audio_playback_underflows(ClientAudioPlayback* playback) {
    if (!playback) return 0;
    std::lock_guard<std::mutex> lock(playback->mutex);
    return playback->underflows;
}
uint64_t client_audio_playback_late_drops(ClientAudioPlayback* playback) {
    if (!playback) return 0;
    std::lock_guard<std::mutex> lock(playback->mutex);
    return playback->late_drops;
}
uint64_t client_audio_playback_discontinuities(ClientAudioPlayback* playback) {
    if (!playback) return 0;
    std::lock_guard<std::mutex> lock(playback->mutex);
    return playback->discontinuities;
}

} // namespace waydisplay
