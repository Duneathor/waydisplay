#pragma once

#include "waydisplay/wd_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_audio_packetizer {
    uint8_t session_id;
    uint64_t connection_token;
    uint64_t audio_epoch;
    uint64_t media_clock_id;
    uint64_t sequence;
    uint64_t expected_pts_samples;
    bool have_expected_pts;
    bool force_discontinuity;
};

void wd_audio_packetizer_begin(struct wd_audio_packetizer* packetizer, uint8_t session_id,
                               uint64_t connection_token, uint64_t audio_epoch,
                               uint64_t media_clock_id);
void wd_audio_packetizer_mark_discontinuity(struct wd_audio_packetizer* packetizer);
bool wd_audio_packetizer_make_packet(struct wd_audio_packetizer* packetizer,
                                     uint64_t pts_samples, uint16_t duration_samples,
                                     uint32_t data_size,
                                     struct wd_audio_packet_payload_header* header);
bool wd_audio_packetizer_make_eos(struct wd_audio_packetizer* packetizer,
                                  uint64_t pts_samples,
                                  struct wd_audio_packet_payload_header* header);

#ifdef __cplusplus
}
#endif
