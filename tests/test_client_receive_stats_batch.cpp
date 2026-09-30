#include "client_receive_stats_batch.hpp"

#include "test_check.h"

int main()
{
    waydisplay::ClientReceiveStatsBatch batch{};

    batch.note_udp_packet(1200);
    batch.note_udp_packet(128);
    batch.note_interarrival(2'000'000);
    batch.note_interarrival(3'000'000);
    batch.note_jitter(1'000'000);
    batch.note_tile_present_queue_depth(3);
    batch.note_tile_present_queue_depth(7);
    batch.note_tile_present_queue_depth(4);
    batch.note_tile_present_overflow();
    batch.note_tile_present_overflow();
    batch.note_lock_wait(900);
    batch.note_lock_wait(2'400);
    batch.note_completed_tile(4096, 4, true, 750'000);
    batch.note_completed_tile(2048, 2, false, 0);
    batch.note_completed_tile(512, 1, true, 0);

    WD_TEST_CHECK(batch.udp_packets_rx == 2);
    WD_TEST_CHECK(batch.udp_bytes_rx == 1328);
    WD_TEST_CHECK(batch.udp_interarrival_samples == 2);
    WD_TEST_CHECK(batch.udp_interarrival_sum_ns == 5'000'000);
    WD_TEST_CHECK(batch.udp_interarrival_max_ns == 3'000'000);
    WD_TEST_CHECK(batch.udp_interarrival_jitter_samples == 1);
    WD_TEST_CHECK(batch.udp_interarrival_jitter_sum_ns == 1'000'000);
    WD_TEST_CHECK(batch.udp_tiles_completed == 3);
    WD_TEST_CHECK(batch.udp_completed_compressed_bytes == 6656);
    WD_TEST_CHECK(batch.udp_completed_packets == 7);
    WD_TEST_CHECK(batch.tile_assembly_samples == 2);
    WD_TEST_CHECK(batch.tile_assembly_sum_ns == 750'000);
    WD_TEST_CHECK(batch.tile_present_queue_depth_max == 7);
    WD_TEST_CHECK(batch.tile_present_overflow_fallbacks == 2);
    WD_TEST_CHECK(batch.lock_wait_samples == 2);
    WD_TEST_CHECK(batch.lock_wait_sum_ns == 3'300);
    WD_TEST_CHECK(batch.lock_wait_max_ns == 2'400);
    return 0;
}
