#include "client_receive_stats_batch.hpp"

#include <cassert>

int main()
{
    waydisplay::ClientReceiveStatsBatch batch{};

    batch.note_udp_packet(1200);
    batch.note_udp_packet(128);
    batch.note_interarrival(2'000'000);
    batch.note_interarrival(3'000'000);
    batch.note_jitter(1'000'000);
    batch.note_completed_tile(4096, 4, true, 750'000);
    batch.note_completed_tile(2048, 2, false, 0);
    batch.note_completed_tile(512, 1, true, 0);

    assert(batch.udp_packets_rx == 2);
    assert(batch.udp_bytes_rx == 1328);
    assert(batch.udp_interarrival_samples == 2);
    assert(batch.udp_interarrival_sum_ns == 5'000'000);
    assert(batch.udp_interarrival_jitter_samples == 1);
    assert(batch.udp_interarrival_jitter_sum_ns == 1'000'000);
    assert(batch.udp_tiles_completed == 3);
    assert(batch.udp_completed_compressed_bytes == 6656);
    assert(batch.udp_completed_packets == 7);
    assert(batch.tile_assembly_samples == 2);
    assert(batch.tile_assembly_sum_ns == 750'000);
    return 0;
}
