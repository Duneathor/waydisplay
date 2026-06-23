#include "wd_audio_packetizer.h"
#include "wd_audio_ring.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    wd_audio_pcm_ring ring{};
    require(wd_audio_pcm_ring_init(&ring, 8, 2), "initialize bounded stereo ring");

    const float first[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    require(wd_audio_pcm_ring_write(&ring, first, 6, 100), "write first block");
    require(wd_audio_pcm_ring_queued_frames(&ring) == 6, "queued frame count");
    require(!wd_audio_pcm_ring_write(&ring, first, 3, 106), "overflow should drop the whole block");
    require(wd_audio_pcm_ring_overruns(&ring) == 1, "overflow telemetry");

    float    output[8]{};
    uint64_t pts        = 0;
    bool     continuous = false;
    require(wd_audio_pcm_ring_read(&ring, output, 4, &pts, &continuous) == 4, "read exact first block");
    require(pts == 100 && continuous, "first block PTS continuity");
    require(output[0] == 0 && output[7] == 7, "interleaved samples should round trip");

    const float second[] = {12, 13, 14, 15, 16, 17, 18, 19};
    require(wd_audio_pcm_ring_write(&ring, second, 4, 200), "write wrapped discontinuous block");
    require(wd_audio_pcm_ring_read(&ring, output, 4, &pts, &continuous) == 4, "read across wrap");
    require(pts == 104 && !continuous, "timestamp gap should signal discontinuity");

    wd_audio_pcm_ring_drop_all(&ring);
    require(wd_audio_pcm_ring_queued_frames(&ring) == 0, "drop-all empties queued capture latency");

    const float  left[]   = {0.25f, 0.5f, 0.75f};
    const float  right[]  = {-0.25f, -0.5f, -0.75f};
    const float* planes[] = {left, right};
    require(wd_audio_pcm_ring_write_planar(&ring, planes, 3, 300), "write PipeWire planar stereo block");
    require(wd_audio_pcm_ring_read(&ring, output, 3, &pts, &continuous) == 3, "read planar block as interleaved PCM");
    require(pts == 300 && continuous, "planar block PTS continuity");
    require(output[0] == left[0] && output[1] == right[0] && output[4] == left[2] && output[5] == right[2],
            "planar channels should interleave without format conversion");
    wd_audio_pcm_ring_reset(&ring);
    require(wd_audio_pcm_ring_queued_frames(&ring) == 0, "reset empties ring");
    wd_audio_pcm_ring_finish(&ring);

    wd_audio_packetizer packetizer{};
    wd_audio_packetizer_begin(&packetizer, 7, 8, 9, 10);
    wd_audio_packet_payload_header header{};
    require(wd_audio_packetizer_make_packet(&packetizer, 0, 960, 20, &header), "packetize first audio frame");
    require(header.sequence == 1 && (header.flags & WD_AUDIO_PACKET_DISCONTINUITY) != 0,
            "first audio frame should establish a discontinuity boundary");
    require(wd_audio_packetizer_make_packet(&packetizer, 960, 960, 20, &header), "packetize continuous audio frame");
    require(header.sequence == 2 && header.flags == 0, "continuous audio should not carry a discontinuity flag");
    require(wd_audio_packetizer_make_packet(&packetizer, 3000, 960, 20, &header), "packetize timestamp gap");
    require((header.flags & WD_AUDIO_PACKET_DISCONTINUITY) != 0, "timestamp gaps should be explicit on the wire");
    require(wd_audio_packetizer_make_eos(&packetizer, 3960, &header), "packetize audio EOS");
    require(header.duration_samples == 0 && header.data_size == 0 && header.flags == WD_AUDIO_PACKET_END_OF_STREAM,
            "audio EOS should be canonical");
    require(wd_audio_packet_payload_size_is_valid(&header, sizeof(header)), "canonical audio EOS should validate");
    header.flags |= WD_AUDIO_PACKET_DISCONTINUITY;
    require(!wd_audio_packet_payload_size_is_valid(&header, sizeof(header)), "audio EOS must reject ambiguous flag combinations");
    return 0;
}
