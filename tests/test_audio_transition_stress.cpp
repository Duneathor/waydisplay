#include "audio_video_sync.hpp"
#include "waydisplay/wd_audio_transport.h"
#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_protocol.h"
#include "wd_audio_packetizer.h"
#include "wd_audio_ring.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

wd_client_hello_payload make_audio_hello(bool enabled) {
    wd_client_hello_payload hello{};
    hello.client_udp_port = 6000;
    hello.video_mode      = WD_VIDEO_MODE_OFF;
    if (enabled)
    {
        hello.capabilities            = WD_CLIENT_CAP_AUDIO_STREAM;
        hello.audio_codecs            = WD_AUDIO_CODEC_OPUS;
        hello.audio_transport         = WD_AUDIO_TRANSPORT_TCP;
        hello.audio_max_channels      = WD_AUDIO_CHANNELS_MAX;
        hello.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;
    }
    return hello;
}

void stress_audio_negotiation_and_opt_out() {
    for (uint32_t iteration = 0; iteration < 100000; ++iteration)
    {
        const bool              enabled = (iteration & 1u) != 0;
        wd_client_hello_payload hello   = make_audio_hello(enabled);
        require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "enabled and opt-out audio hellos must both be canonical");
        if (!enabled)
        {
            require((hello.capabilities & WD_CLIENT_CAP_AUDIO_STREAM) == 0 && hello.audio_codecs == 0 && hello.audio_transport == 0 &&
                        hello.audio_max_channels == 0 && hello.audio_target_latency_ms == 0,
                    "audio opt-out must suppress all audio negotiation fields");
        }

        wd_client_hello_payload malformed = hello;
        malformed.audio_codecs            = WD_AUDIO_CODEC_OPUS;
        if (!enabled)
        {
            require(!wd_client_hello_payload_is_valid(&malformed, sizeof(malformed)), "disabled audio must reject residual codec fields");
        }
    }
}

void stress_packet_epochs_and_discontinuities() {
    std::mt19937_64     random(0xa0d10f00dull);
    uint8_t             session           = 1;
    uint64_t            token             = 0x1000;
    uint64_t            epoch             = 1;
    uint64_t            clock             = 0x2000;
    uint64_t            pts               = 0;
    uint64_t            previous_sequence = 0;
    wd_audio_packetizer packetizer{};
    wd_audio_packetizer_begin(&packetizer, session, token, epoch, clock);

    for (uint32_t iteration = 0; iteration < 100000; ++iteration)
    {
        if ((random() % 997u) == 0)
        {
            session = static_cast<uint8_t>(session == 255 ? 1 : session + 1);
            token++;
            epoch++;
            clock++;
            pts               = 0;
            previous_sequence = 0;
            wd_audio_packetizer_begin(&packetizer, session, token, epoch, clock);
        }

        const bool explicit_gap = (random() % 211u) == 0;
        if (explicit_gap)
        {
            pts += WD_AUDIO_FRAME_SAMPLES_DEFAULT;
        }
        const bool forced = (random() % 307u) == 0;
        if (forced)
        {
            wd_audio_packetizer_mark_discontinuity(&packetizer);
        }

        wd_audio_packet_payload_header header{};
        const uint32_t                 data_size = 20u + static_cast<uint32_t>(random() % 400u);
        require(wd_audio_packetizer_make_packet(&packetizer, pts, WD_AUDIO_FRAME_SAMPLES_DEFAULT, data_size, &header),
                "valid Opus packets must be packetized");
        require(header.session_id == session && header.connection_token == token && header.audio_epoch == epoch &&
                    header.media_clock_id == clock,
                "packet identity must follow the current connection and audio epoch");
        require(header.sequence == previous_sequence + 1, "audio packet sequence must be strictly increasing within an epoch");
        require(wd_audio_packet_payload_size_is_valid(&header, static_cast<uint32_t>(sizeof(header)) + header.data_size),
                "packetizer output must pass wire validation");
        if (previous_sequence == 0 || explicit_gap || forced)
        {
            require((header.flags & WD_AUDIO_PACKET_DISCONTINUITY) != 0, "new epochs and timeline gaps must be explicit");
        }
        previous_sequence = header.sequence;
        pts += WD_AUDIO_FRAME_SAMPLES_DEFAULT;
    }
}

void stress_ring_wrap_and_overrun() {
    wd_audio_pcm_ring ring{};
    require(wd_audio_pcm_ring_init(&ring, 4096, 2), "initialize stress audio ring");
    std::vector<float> input(960u * 2u);
    std::vector<float> output(960u * 2u);
    uint64_t           write_pts         = 0;
    uint64_t           expected_read_pts = 0;
    uint64_t           successful_writes = 0;
    uint64_t           successful_reads  = 0;

    for (uint32_t iteration = 0; iteration < 50000; ++iteration)
    {
        for (size_t index = 0; index < input.size(); ++index)
        {
            input[index] = static_cast<float>((write_pts + index) & 0xffu);
        }
        if (wd_audio_pcm_ring_write(&ring, input.data(), 960, write_pts))
        {
            successful_writes++;
            write_pts += 960;
        }

        if ((iteration % 3u) != 0)
        {
            uint64_t read_pts   = 0;
            bool     continuous = false;
            if (wd_audio_pcm_ring_read(&ring, output.data(), 960, &read_pts, &continuous) == 960)
            {
                require(read_pts == expected_read_pts && continuous, "ring wrap must preserve sample timestamps");
                expected_read_pts += 960;
                successful_reads++;
            }
        }
    }

    require(successful_writes != 0 && successful_reads != 0, "stress ring must exercise both producer and consumer");
    require(wd_audio_pcm_ring_overruns(&ring) != 0, "bounded ring must report sustained producer pressure");
    require(wd_audio_pcm_ring_queued_frames(&ring) <= ring.capacity_frames, "ring occupancy must remain bounded");
    wd_audio_pcm_ring_finish(&ring);
}

void stress_media_clock_sync_and_budget() {
    uint64_t previous_samples = 0;
    for (uint64_t usec = 0; usec < 10000000ull; usec += 137ull)
    {
        const uint64_t samples = wd_media_usec_to_samples(usec, WD_AUDIO_SAMPLE_RATE_DEFAULT);
        require(samples >= previous_samples, "media sample clock must be monotonic");
        previous_samples = samples;

        const ClientVideoAudioSyncDecision decision = client_video_audio_sync_decide(usec, samples, WD_AUDIO_SAMPLE_RATE_DEFAULT);
        require(decision == ClientVideoAudioSyncDecision::Present, "matching audio/video timestamps should present immediately");
    }

    std::mt19937_64 random(0xbad6e7ull);
    for (uint32_t iteration = 0; iteration < 100000; ++iteration)
    {
        const uint64_t measured    = 1u + random() % (64ull * 1024ull * 1024ull);
        const uint32_t bitrate     = 6000u + static_cast<uint32_t>(random() % 504000u);
        const uint64_t tile_budget = wd_audio_reserve_from_tile_budget(measured, bitrate);
        require(tile_budget != 0 && tile_budget <= measured, "audio reservation must preserve a bounded nonzero tile budget");
    }
}

} // namespace

int main() {
    stress_audio_negotiation_and_opt_out();
    stress_packet_epochs_and_discontinuities();
    stress_ring_wrap_and_overrun();
    stress_media_clock_sync_and_budget();
    return 0;
}
