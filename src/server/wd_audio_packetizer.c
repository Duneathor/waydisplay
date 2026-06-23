#include "wd_audio_packetizer.h"

#include <string.h>

void wd_audio_packetizer_begin(struct wd_audio_packetizer* packetizer, uint8_t session_id, uint64_t connection_token, uint64_t audio_epoch,
                               uint64_t media_clock_id) {
    if (!packetizer)
    {
        return;
    }
    memset(packetizer, 0, sizeof(*packetizer));
    packetizer->session_id          = session_id;
    packetizer->connection_token    = connection_token;
    packetizer->audio_epoch         = audio_epoch;
    packetizer->media_clock_id      = media_clock_id;
    packetizer->force_discontinuity = true;
}

void wd_audio_packetizer_mark_discontinuity(struct wd_audio_packetizer* packetizer) {
    if (packetizer)
    {
        packetizer->force_discontinuity = true;
        packetizer->have_expected_pts   = false;
    }
}

bool wd_audio_packetizer_make_packet(struct wd_audio_packetizer* packetizer, uint64_t pts_samples, uint16_t duration_samples,
                                     uint32_t data_size, struct wd_audio_packet_payload_header* header) {
    if (!packetizer || !header || packetizer->session_id == 0 || packetizer->connection_token == 0 || packetizer->audio_epoch == 0 ||
        packetizer->media_clock_id == 0 || !wd_audio_frame_samples_is_valid(duration_samples) || data_size == 0 ||
        data_size > WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES)
    {
        return false;
    }
    memset(header, 0, sizeof(*header));
    header->session_id       = packetizer->session_id;
    header->connection_token = packetizer->connection_token;
    header->audio_epoch      = packetizer->audio_epoch;
    header->media_clock_id   = packetizer->media_clock_id;
    header->sequence         = ++packetizer->sequence;
    header->pts_samples      = pts_samples;
    header->duration_samples = duration_samples;
    header->data_size        = data_size;
    if (packetizer->force_discontinuity || (packetizer->have_expected_pts && pts_samples != packetizer->expected_pts_samples))
    {
        header->flags |= WD_AUDIO_PACKET_DISCONTINUITY;
    }
    packetizer->force_discontinuity  = false;
    packetizer->have_expected_pts    = true;
    packetizer->expected_pts_samples = pts_samples + duration_samples;
    return true;
}

bool wd_audio_packetizer_make_eos(struct wd_audio_packetizer* packetizer, uint64_t pts_samples,
                                  struct wd_audio_packet_payload_header* header) {
    if (!packetizer || !header || packetizer->session_id == 0 || packetizer->connection_token == 0 || packetizer->audio_epoch == 0 ||
        packetizer->media_clock_id == 0)
    {
        return false;
    }
    memset(header, 0, sizeof(*header));
    header->session_id              = packetizer->session_id;
    header->connection_token        = packetizer->connection_token;
    header->audio_epoch             = packetizer->audio_epoch;
    header->media_clock_id          = packetizer->media_clock_id;
    header->sequence                = ++packetizer->sequence;
    header->pts_samples             = pts_samples;
    header->flags                   = WD_AUDIO_PACKET_END_OF_STREAM;
    packetizer->have_expected_pts   = false;
    packetizer->force_discontinuity = true;
    return true;
}
