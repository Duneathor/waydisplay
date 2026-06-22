#include "audio_playback.hpp"
#include "audio_playback_clock.hpp"

#include "waydisplay/wd_log.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
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
    SDL_AudioDeviceID device_id = 0;
    std::atomic<bool> device_clock_active{false};
    std::atomic<uint64_t> device_mixed_samples_fp{0};
    std::atomic<uint64_t> device_buffer_samples{0};
    std::atomic<uint64_t> device_clock_limit_samples{0};
    std::atomic<bool> device_starved{false};
#if WAYDISPLAY_HAVE_OPUS_AUDIO
    OpusDecoder* decoder = nullptr;
#endif
    wd_audio_config_payload config{};
    bool configured = false;
    bool playing = false;
    bool video_sync_waiting = false;
    uint64_t expected_sequence = 0;
    uint64_t expected_pts_samples = 0;
    bool have_expected_pts = false;
    uint64_t playback_start_pts = 0;
    bool have_playback_start_pts = false;
    uint64_t submitted_end_pts = 0;
    uint16_t target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;
    uint16_t pre_skip_remaining = 0;
    uint64_t underflows = 0;
    uint64_t late_drops = 0;
    uint64_t discontinuities = 0;
    std::vector<float> decode_buffer;
};

namespace {

constexpr int OPUS_MAX_DECODE_SAMPLES = 5760;

void SDLCALL audio_postmix_callback(void* userdata, const SDL_AudioSpec* spec,
                                    float* buffer, int buflen) {
    (void)buffer;
    auto* playback = static_cast<ClientAudioPlayback*>(userdata);
    if (!playback || !spec || spec->channels <= 0 || spec->freq <= 0 || buflen <= 0 ||
        !playback->device_clock_active.load(std::memory_order_acquire))
    {
        return;
    }

    const uint64_t bytes_per_frame = sizeof(float) *
                                     static_cast<uint64_t>(spec->channels);
    const uint64_t frames = static_cast<uint64_t>(buflen) / bytes_per_frame;
    const uint32_t sample_rate = playback->config.sample_rate;
    const uint64_t mixed_samples_fp = client_audio_frames_to_samples_fp(
        frames, static_cast<uint32_t>(spec->freq), sample_rate);
    const uint64_t buffer_samples =
        mixed_samples_fp >> CLIENT_AUDIO_CLOCK_FRACTION_BITS;
    playback->device_buffer_samples.store(buffer_samples,
                                          std::memory_order_release);
    const uint64_t mixed_total_fp =
        playback->device_mixed_samples_fp.fetch_add(mixed_samples_fp,
                                                     std::memory_order_acq_rel) +
        mixed_samples_fp;
    const uint64_t limit = playback->device_clock_limit_samples.load(
        std::memory_order_acquire);
    if (limit != 0 &&
        (mixed_total_fp >> CLIENT_AUDIO_CLOCK_FRACTION_BITS) >=
            limit + buffer_samples)
    {
        playback->device_starved.store(true, std::memory_order_release);
    }
}

void publish_device_clock_limit_locked(ClientAudioPlayback* playback) {
    uint64_t limit = 0;
    if (playback && playback->have_playback_start_pts &&
        playback->submitted_end_pts >= playback->playback_start_pts)
    {
        limit = playback->submitted_end_pts - playback->playback_start_pts;
    }
    if (playback)
    {
        playback->device_clock_limit_samples.store(limit,
                                                    std::memory_order_release);
    }
}

uint64_t device_playhead_locked(const ClientAudioPlayback* playback) {
    if (!playback || !playback->have_playback_start_pts)
    {
        return 0;
    }
    return client_audio_device_playhead(
        playback->playback_start_pts, playback->submitted_end_pts,
        playback->device_mixed_samples_fp.load(std::memory_order_acquire),
        playback->device_buffer_samples.load(std::memory_order_acquire));
}

void reset_device_clock_locked(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    playback->device_clock_active.store(false, std::memory_order_release);
    if (playback->device_id != 0)
    {
        /* SDL waits for an in-flight postmix callback before returning. This
         * prevents a callback from the previous playback activation from
         * updating the next activation's presentation counters. */
        (void)SDL_SetAudioPostmixCallback(playback->device_id, nullptr, nullptr);
    }
    playback->device_mixed_samples_fp.store(0, std::memory_order_release);
    playback->device_clock_limit_samples.store(0, std::memory_order_release);
    playback->device_starved.store(false, std::memory_order_release);
}

bool handle_device_starvation_locked(ClientAudioPlayback* playback) {
    if (!playback || !playback->playing || !playback->have_playback_start_pts)
    {
        return false;
    }

    const bool callback_reported = playback->device_starved.exchange(
        false, std::memory_order_acq_rel);
    const uint64_t mixed_samples_fp = playback->device_mixed_samples_fp.load(
        std::memory_order_acquire);
    const uint64_t buffer_samples = playback->device_buffer_samples.load(
        std::memory_order_acquire);
    const bool consumed = client_audio_device_consumed(
        playback->playback_start_pts, playback->submitted_end_pts,
        mixed_samples_fp, buffer_samples);
    if (!callback_reported && !consumed)
    {
        return false;
    }
    if (!consumed)
    {
        return false;
    }

    SDL_PauseAudioStreamDevice(playback->stream);
    SDL_ClearAudioStream(playback->stream);
    playback->playing = false;
    playback->video_sync_waiting = false;
    playback->underflows++;
    reset_device_clock_locked(playback);
    playback->playback_start_pts = 0;
    playback->have_playback_start_pts = false;
    return true;
}

void destroy_stream_locked(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    reset_device_clock_locked(playback);
    playback->device_id = 0;
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
    playback->video_sync_waiting = false;
    playback->expected_sequence = 0;
    playback->expected_pts_samples = 0;
    playback->have_expected_pts = false;
    playback->playback_start_pts = 0;
    playback->have_playback_start_pts = false;
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

void clear_for_output_gap_locked(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return;
    }
    if (playback->stream)
    {
        SDL_ClearAudioStream(playback->stream);
        SDL_PauseAudioStreamDevice(playback->stream);
    }
    playback->playing = false;
    playback->video_sync_waiting = false;
    reset_device_clock_locked(playback);
    playback->playback_start_pts = 0;
    playback->have_playback_start_pts = false;
    playback->submitted_end_pts = 0;
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
    playback->video_sync_waiting = true;
    reset_device_clock_locked(playback);
    playback->playback_start_pts = 0;
    playback->have_playback_start_pts = false;
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
    playback->device_id = SDL_GetAudioStreamDevice(playback->stream);
    SDL_AudioSpec device_spec{};
    int device_buffer_frames = 0;
    if (playback->device_id == 0 ||
        !SDL_GetAudioDeviceFormat(playback->device_id, &device_spec,
                                  &device_buffer_frames))
    {
        WD_LOG_ERROR("failed to configure SDL audio presentation clock: %s",
                     SDL_GetError());
        destroy_stream_locked(playback);
        return false;
    }
    playback->device_buffer_samples.store(
        client_audio_frames_to_samples_fp(
            device_buffer_frames > 0 ? static_cast<uint64_t>(device_buffer_frames) : 0,
            device_spec.freq > 0 ? static_cast<uint32_t>(device_spec.freq) : config.sample_rate,
            config.sample_rate) >> CLIENT_AUDIO_CLOCK_FRACTION_BITS,
        std::memory_order_release);

    playback->target_latency_ms = target_latency_ms;
    playback->pre_skip_remaining = config.codec_delay_samples;
    playback->configured = true;
    playback->video_sync_waiting = true;
    playback->decode_buffer.resize(static_cast<size_t>(OPUS_MAX_DECODE_SAMPLES) * config.channels);
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

    const bool sequence_gap = playback->expected_sequence != 0 &&
                              header.sequence != playback->expected_sequence;
    const bool pts_gap = playback->have_expected_pts &&
                         header.pts_samples != playback->expected_pts_samples;
    const bool wire_discontinuity =
        (header.flags & WD_AUDIO_PACKET_DISCONTINUITY) != 0;
    if (wire_discontinuity || sequence_gap || pts_gap)
    {
        /* Process the current packet as the first packet of the new timeline.
         * Dropping it would make the following packet fail sequence/PTS checks
         * as well and turn one gap into a cascading recovery. */
        clear_for_discontinuity_locked(playback);
    }

    if ((header.flags & WD_AUDIO_PACKET_END_OF_STREAM) != 0)
    {
        playback->expected_sequence = header.sequence + 1;
        if (playback->stream)
        {
            SDL_PauseAudioStreamDevice(playback->stream);
        }
        playback->playing = false;
        playback->video_sync_waiting = false;
        reset_device_clock_locked(playback);
        return true;
    }

#if !WAYDISPLAY_HAVE_OPUS_AUDIO
    return false;
#else
    if (header.duration_samples != playback->config.frame_samples ||
        UINT64_MAX - header.pts_samples < header.duration_samples)
    {
        return false;
    }

    const uint8_t* packet = payload + sizeof(header);
    const int decoded = opus_decode_float(playback->decoder, packet,
                                          static_cast<opus_int32>(header.data_size),
                                          playback->decode_buffer.data(), OPUS_MAX_DECODE_SAMPLES, 0);
    if (decoded <= 0 || decoded != header.duration_samples)
    {
        clear_for_discontinuity_locked(playback);
        return false;
    }

    const uint64_t packet_end_pts = header.pts_samples +
                                    static_cast<uint64_t>(decoded);
    const uint16_t skip = static_cast<uint16_t>(std::min<int>(
        decoded, playback->pre_skip_remaining));
    playback->pre_skip_remaining = static_cast<uint16_t>(
        playback->pre_skip_remaining - skip);

    (void)handle_device_starvation_locked(playback);
    const uint64_t current_playhead = device_playhead_locked(playback);
    const uint64_t late_limit = (WD_AUDIO_SAMPLE_RATE_DEFAULT * WD_CLIENT_AUDIO_LATE_PACKET_MS) / WD_MSEC_PER_SEC;
    const bool late = playback->playing &&
                      packet_end_pts <= UINT64_MAX - late_limit &&
                      packet_end_pts + late_limit < current_playhead;
    if (late)
    {
        /* Opus is stateful. Decode the late packet to advance the decoder, but
         * discard its PCM and move the expected wire timeline forward. Reset
         * only the output queue so the next packet starts a fresh SDL playback
         * anchor without inventing a sender-side codec reset. */
        playback->expected_sequence = header.sequence + 1;
        playback->expected_pts_samples = packet_end_pts;
        playback->have_expected_pts = true;
        playback->late_drops++;
        clear_for_output_gap_locked(playback);
        return true;
    }

    const int queued_frames = decoded - skip;
    if (queued_frames > 0)
    {
        if (!playback->have_playback_start_pts)
        {
            playback->playback_start_pts = header.pts_samples + skip;
            playback->have_playback_start_pts = true;
        }
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
    playback->submitted_end_pts = packet_end_pts;
    publish_device_clock_limit_locked(playback);
    playback->expected_sequence = header.sequence + 1;
    playback->expected_pts_samples = packet_end_pts;
    playback->have_expected_pts = true;

    const uint64_t target_samples =
        (static_cast<uint64_t>(playback->config.sample_rate) *
         playback->target_latency_ms) / 1000u;
    if (!playback->playing && playback->have_playback_start_pts &&
        queued_samples_locked(playback) >= target_samples)
    {
        playback->device_mixed_samples_fp.store(0, std::memory_order_release);
        if (!SDL_SetAudioPostmixCallback(playback->device_id,
                                         audio_postmix_callback, playback))
        {
            WD_LOG_ERROR("failed to start SDL audio presentation clock: %s",
                         SDL_GetError());
        }
        else
        {
            playback->device_clock_active.store(true, std::memory_order_release);
            if (SDL_ResumeAudioStreamDevice(playback->stream))
            {
                playback->playing = true;
                playback->video_sync_waiting = false;
            }
            else
            {
                reset_device_clock_locked(playback);
            }
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
    (void)handle_device_starvation_locked(playback);
    return playback->playing;
}

bool client_audio_playback_should_hold_video(ClientAudioPlayback* playback) {
    if (!playback)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(playback->mutex);
    (void)handle_device_starvation_locked(playback);
    return playback->configured && playback->video_sync_waiting;
}

bool client_audio_playback_playhead_samples(ClientAudioPlayback* playback,
                                            uint64_t* playhead_samples) {
    if (!playback || !playhead_samples)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(playback->mutex);
    (void)handle_device_starvation_locked(playback);
    if (!playback->configured || !playback->playing)
    {
        return false;
    }
    *playhead_samples = device_playhead_locked(playback);
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
