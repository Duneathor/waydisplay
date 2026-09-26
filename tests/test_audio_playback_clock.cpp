#include "audio_playback_clock.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void test_output_rebase_boundaries() {
    const uint64_t max_queue = client_audio_max_queued_samples(48000, 20);
    require(max_queue == 5760, "20 ms startup latency should retain a 120 ms lifetime queue bound");
    require(!client_audio_output_rebase_needed(false, max_queue + 1, 960, max_queue),
            "startup buffering must not trigger an output-only rebase");
    require(!client_audio_output_rebase_needed(true, max_queue - 960, 960, max_queue),
            "a packet that lands exactly on the queue bound must be accepted");
    require(client_audio_output_rebase_needed(true, max_queue - 959, 960, max_queue),
            "a packet that exceeds the queue bound by one sample must rebase");
    require(client_audio_output_rebase_needed(true, max_queue + 1, 1, max_queue),
            "an already excessive running queue must rebase without subtraction underflow");
}

void test_rebase_simulation_preserves_wire_cursor_and_reanchors_output() {
    const uint64_t max_queue = client_audio_max_queued_samples(48000, 20);
    uint64_t wire_sequence = 100;
    uint64_t wire_pts = 48000;
    uint64_t output_start_pts = wire_pts;
    uint64_t output_queue = max_queue - 100;

    const uint64_t incoming = 960;
    require(client_audio_output_rebase_needed(true, output_queue, incoming, max_queue),
            "the simulated backlog must request a rebase");

    const uint64_t packet_sequence = wire_sequence;
    const uint64_t packet_pts = wire_pts;
    output_queue = 0;
    output_start_pts = packet_pts;
    output_queue += incoming;
    wire_sequence = packet_sequence + 1;
    wire_pts = packet_pts + incoming;

    require(output_start_pts == 48000,
            "the replacement output anchor must be the already-decoded packet being queued");
    require(output_queue == incoming, "rebasing must discard stale output before accepting the current packet");
    require(wire_sequence == 101 && wire_pts == 48960,
            "output rebasing must not reset or skip the sender sequence/PTS cursor");
}

void test_postmix_silence_is_bounded_by_stream_queue() {
    const uint64_t mixed = client_audio_frames_to_samples_fp(9600, 48000, 48000);
    require(client_audio_device_playhead_queued(48000, 52800, mixed, 480, 2880) == 49440,
            "postmix silence must not advance through queued stream PCM");
    require(client_audio_device_playhead_queued(48000, 52800, mixed, 480, 0) == 52320,
            "a drained stream may advance to the device-buffer edge");
    require(client_audio_device_playhead_queued(48000, 52800, 0, 480, 2880) == 48000,
            "an inactive device clock must remain at the playback anchor");
    require(client_audio_device_playhead_queued(48000, 52800, mixed, 480, UINT64_MAX) == 48000,
            "impossible queued counts must clamp instead of underflowing");
}

void test_starvation_requires_drain_and_counts_once() {
    const uint64_t mixed = client_audio_frames_to_samples_fp(5280, 48000, 48000);
    const bool consumed = client_audio_device_consumed(48000, 52800, mixed, 480);
    require(consumed, "fixture must represent a consumed submitted media range");

    require(!client_audio_device_starvation_confirmed(true, true, consumed, 960),
            "postmix progress cannot starve a stream that still owns queued PCM");
    require(!client_audio_device_starvation_confirmed(false, true, consumed, 0),
            "an already stopped stream must not report another underflow");
    require(!client_audio_device_starvation_confirmed(true, false, consumed, 0),
            "a stream without a playback anchor cannot underflow");
    require(client_audio_device_starvation_confirmed(true, true, consumed, 0),
            "a running anchored stream is starved once its submitted media is consumed and queue is empty");

    bool playing = true;
    uint64_t underflows = 0;
    if (client_audio_device_starvation_confirmed(playing, true, consumed, 0))
    {
        playing = false;
        ++underflows;
    }
    if (client_audio_device_starvation_confirmed(playing, true, consumed, 0))
    {
        ++underflows;
    }
    require(underflows == 1, "the same drained range must increment the underflow counter exactly once");
}

void test_fractional_device_rate_accumulates_without_truncation() {
    const uint64_t one_second = client_audio_frames_to_samples_fp(44100, 44100, 48000);
    require((one_second >> CLIENT_AUDIO_CLOCK_FRACTION_BITS) == 48000,
            "device-rate conversion must preserve one-second media duration");

    const uint64_t one_frame = client_audio_frames_to_samples_fp(1, 44100, 48000);
    require(one_frame > CLIENT_AUDIO_CLOCK_FRACTION_ONE,
            "44.1 kHz device frames must retain the fractional 48 kHz media-sample increment");
}

} // namespace

int main() {
    test_output_rebase_boundaries();
    test_rebase_simulation_preserves_wire_cursor_and_reanchors_output();
    test_postmix_silence_is_bounded_by_stream_queue();
    test_starvation_requires_drain_and_counts_once();
    test_fractional_device_rate_accumulates_without_truncation();
    return EXIT_SUCCESS;
}
