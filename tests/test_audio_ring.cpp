#include "wd_audio_packetizer.h"
#include "wd_audio_ring.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}
void test_concurrent_spsc_publication() {
    constexpr uint32_t capacity = 64;
    constexpr uint64_t frame_count = 100000;
    constexpr uint64_t pts_base = 1000000;

    wd_audio_pcm_ring ring{};
    require(wd_audio_pcm_ring_init(&ring, capacity, 2), "initialize concurrent SPSC ring");

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};

    std::thread producer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (uint64_t index = 0; index < frame_count && !failed.load(std::memory_order_relaxed); ++index)
        {
            const float sample[2] = {static_cast<float>(index), -static_cast<float>(index)};
            while (!wd_audio_pcm_ring_write(&ring, sample, 1, pts_base + index))
            {
                if (failed.load(std::memory_order_relaxed))
                {
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (uint64_t index = 0; index < frame_count; ++index)
        {
            float output[2]{};
            uint64_t pts = 0;
            bool continuous = false;
            while (wd_audio_pcm_ring_read(&ring, output, 1, &pts, &continuous) == 0)
            {
                if (failed.load(std::memory_order_relaxed))
                {
                    return;
                }
                std::this_thread::yield();
            }
            if (pts != pts_base + index || !continuous ||
                output[0] != static_cast<float>(index) || output[1] != -static_cast<float>(index))
            {
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    require(!failed.load(std::memory_order_acquire), "concurrent SPSC samples and PTS stay paired across wraps");
    require(wd_audio_pcm_ring_queued_frames(&ring) == 0, "concurrent SPSC ring drains completely");
    wd_audio_pcm_ring_finish(&ring);
}

} // namespace

int main() {
    test_concurrent_spsc_publication();
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

    wd_audio_pcm_ring mono{};
    require(wd_audio_pcm_ring_init(&mono, 8, 1), "initialize bounded mono ring");
    const float stereo_capture[] = {1.0f, -1.0f, 0.75f, 0.25f, -0.5f, -0.25f};
    require(wd_audio_pcm_ring_write_capture(&mono, stereo_capture, 3, 2, false, 400),
            "stereo capture should downmix into negotiated mono");
    float mono_output[3]{};
    require(wd_audio_pcm_ring_read(&mono, mono_output, 3, &pts, &continuous) == 3, "read downmixed mono capture");
    require(mono_output[0] == 0.0f && mono_output[1] == 0.5f && mono_output[2] == -0.375f,
            "mono capture should average left and right channels per frame");
    require(wd_audio_pcm_ring_write_capture(&mono, nullptr, 2, 2, true, 500), "empty capture chunks should become silence");
    float silence[2] = {1.0f, 1.0f};
    require(wd_audio_pcm_ring_read(&mono, silence, 2, &pts, &continuous) == 2, "read synthesized silent capture");
    require(silence[0] == 0.0f && silence[1] == 0.0f, "silent capture must not expose stale buffer contents");
    wd_audio_pcm_ring_finish(&mono);

    wd_audio_packetizer packetizer{};
    wd_audio_packetizer_begin(&packetizer, 7, 8, 9, 10);
    wd_audio_packet_payload_header header{};
    require(wd_audio_packetizer_make_packet(&packetizer, 0, 960, 20, &header), "packetize first audio frame");
    require(header.sequence == 1 && (header.flags & WD_AUDIO_PACKET_DISCONTINUITY) != 0,
            "first audio frame should establish a discontinuity boundary");
    require(wd_audio_packetizer_make_packet(&packetizer, 960, 960, 20, &header), "packetize continuous audio frame");
    require(header.sequence == 2 && header.flags == 0, "continuous audio should not carry a discontinuity flag");
    packetizer.sequence = UINT64_MAX;
    require(!wd_audio_packetizer_make_packet(&packetizer, 1920, 960, 20, &header), "packetizer sequence must never wrap to zero");
    packetizer.sequence = 2;
    require(!wd_audio_packetizer_make_packet(&packetizer, UINT64_MAX - 100, 960, 20, &header),
            "packetizer must reject PTS ranges that overflow");
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
