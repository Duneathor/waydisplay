#pragma once

#include "waydisplay/wd_protocol.h"

#include <cstdint>

namespace waydisplay {

struct ClientAudioPlayback;

bool        client_audio_playback_create(ClientAudioPlayback** out_playback);
void        client_audio_playback_destroy(ClientAudioPlayback* playback);
bool        client_audio_playback_available();
const char* client_audio_playback_backend_name();

bool     client_audio_playback_configure(ClientAudioPlayback* playback, const wd_audio_config_payload& config, uint16_t target_latency_ms);
bool     client_audio_playback_handle_packet(ClientAudioPlayback* playback, const uint8_t* payload, uint32_t payload_size);
void     client_audio_playback_reset(ClientAudioPlayback* playback);
bool     client_audio_playback_is_configured(ClientAudioPlayback* playback);
bool     client_audio_playback_is_playing(ClientAudioPlayback* playback);
bool     client_audio_playback_video_gate(ClientAudioPlayback* playback, uint64_t now_ns, uint32_t* hold_age_ms, bool* timed_out);
uint8_t  client_audio_playback_state(ClientAudioPlayback* playback);
bool     client_audio_playback_playhead_samples(ClientAudioPlayback* playback, uint64_t* playhead_samples);
uint64_t client_audio_playback_underflows(ClientAudioPlayback* playback);
uint64_t client_audio_playback_late_drops(ClientAudioPlayback* playback);
uint64_t client_audio_playback_discontinuities(ClientAudioPlayback* playback);

} // namespace waydisplay
