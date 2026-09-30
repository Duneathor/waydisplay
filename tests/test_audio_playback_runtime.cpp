#include "audio_playback.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_time.h"

#include <SDL3/SDL.h>
#include <opus/opus.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                    \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

int main() {
    (void)setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (!SDL_Init(SDL_INIT_AUDIO))
    {
        std::fprintf(stderr, "SKIP: SDL dummy audio unavailable: %s\n", SDL_GetError());
        return 77;
    }

    waydisplay::ClientAudioPlayback* playback = nullptr;
    CHECK(waydisplay::client_audio_playback_create(&playback));
    CHECK(playback != nullptr);
    CHECK(waydisplay::client_audio_playback_available());

    int opus_error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(WD_AUDIO_SAMPLE_RATE_DEFAULT, 1, OPUS_APPLICATION_AUDIO, &opus_error);
    if (!encoder || opus_error != OPUS_OK)
    {
        std::fprintf(stderr, "SKIP: Opus encoder unavailable: %d\n", opus_error);
        waydisplay::client_audio_playback_destroy(playback);
        SDL_Quit();
        return 77;
    }

    opus_int32 lookahead = 0;
    CHECK(opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) == OPUS_OK);
    CHECK(lookahead >= 0 && lookahead <= 960);
    CHECK(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000)) == OPUS_OK);

    wd_audio_config_payload config{};
    config.session_id = 7;
    config.connection_token = UINT64_C(0x123456789abcdef0);
    config.audio_epoch = 3;
    config.media_clock_id = 9;
    config.codec = WD_AUDIO_CODEC_OPUS;
    config.sample_rate = WD_AUDIO_SAMPLE_RATE_DEFAULT;
    config.channels = 1;
    config.frame_samples = 960;
    config.codec_delay_samples = static_cast<uint16_t>(lookahead);
    config.target_bitrate = 64000;
    /* Keep the startup gate in its buffering phase after a single Opus packet.
     * A 10 ms target can be satisfied by one 20 ms packet (even after Opus
     * pre-skip), so the dummy backend may already be playing by the time the
     * gate is queried.  Two default latency intervals are strictly larger than
     * one configured packet and make this test exercise HOLD -> TIMEOUT. */
    constexpr uint16_t test_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT * 2u;
    CHECK(test_target_latency_ms <= WD_AUDIO_TARGET_LATENCY_MS_MAX);
    CHECK(waydisplay::client_audio_playback_configure(playback, config, test_target_latency_ms));
    CHECK(waydisplay::client_audio_playback_is_configured(playback));
    CHECK(waydisplay::client_audio_playback_state(playback) == WD_CLIENT_AUDIO_PLAYBACK_STARVED);

    std::vector<float> pcm(config.frame_samples, 0.0f);
    std::vector<uint8_t> encoded(4000);
    const int encoded_bytes = opus_encode_float(encoder, pcm.data(), config.frame_samples, encoded.data(),
                                                 static_cast<opus_int32>(encoded.size()));
    CHECK(encoded_bytes > 0);

    wd_audio_packet_payload_header header{};
    header.session_id = config.session_id;
    header.connection_token = config.connection_token;
    header.audio_epoch = config.audio_epoch;
    header.media_clock_id = config.media_clock_id;
    header.sequence = 1;
    header.pts_samples = 0;
    header.duration_samples = config.frame_samples;
    header.flags = WD_AUDIO_PACKET_DISCONTINUITY;
    header.data_size = static_cast<uint32_t>(encoded_bytes);

    std::vector<uint8_t> payload(sizeof(header) + static_cast<size_t>(encoded_bytes));
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), encoded.data(), static_cast<size_t>(encoded_bytes));
    CHECK(waydisplay::client_audio_playback_handle_packet(playback, payload.data(), static_cast<uint32_t>(payload.size())));

    uint32_t hold_age_ms = 0;
    bool timed_out = false;
    const uint64_t now_ns = wd_now_ns();
    CHECK(waydisplay::client_audio_playback_video_gate(playback, now_ns, &hold_age_ms, &timed_out));
    CHECK(!timed_out);
    CHECK(waydisplay::client_audio_playback_state(playback) == WD_CLIENT_AUDIO_PLAYBACK_BUFFERING ||
          waydisplay::client_audio_playback_state(playback) == WD_CLIENT_AUDIO_PLAYBACK_PLAYING);

    if (!waydisplay::client_audio_playback_is_playing(playback))
    {
        const uint64_t timeout_ns = now_ns +
            (static_cast<uint64_t>(WD_CLIENT_AUDIO_VIDEO_STARTUP_HOLD_MAX_MS) + 1u) * WD_NSEC_PER_MSEC;
        CHECK(!waydisplay::client_audio_playback_video_gate(playback, timeout_ns, &hold_age_ms, &timed_out));
        CHECK(timed_out);
        CHECK(hold_age_ms >= WD_CLIENT_AUDIO_VIDEO_STARTUP_HOLD_MAX_MS);
    }

    waydisplay::client_audio_playback_reset(playback);
    CHECK(!waydisplay::client_audio_playback_is_configured(playback));
    CHECK(waydisplay::client_audio_playback_state(playback) == WD_CLIENT_AUDIO_PLAYBACK_DISABLED);

    opus_encoder_destroy(encoder);
    waydisplay::client_audio_playback_destroy(playback);
    SDL_Quit();
    return 0;
}
