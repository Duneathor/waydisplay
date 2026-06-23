#include "wd_async_udp_accounting.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_partial_submit_is_not_counted_as_fully_submitted() {
    wd_async_udp_accounting accounting{};
    for (int i = 0; i < 4; ++i)
    {
        wd_async_udp_accounting_queue(&accounting);
    }

    require(wd_async_udp_accounting_submit_result(&accounting, 2) == 2, "partial submit count");
    require(accounting.prepared == 2, "unsubmitted packets must remain prepared");
    require(accounting.submitted == 2, "only submitted packets become in flight");
    require(wd_async_udp_accounting_pending(&accounting) == 4, "all packets remain pending");

    require(wd_async_udp_accounting_submit_result(&accounting, 8) == 2, "submit result must clamp to prepared count");
    require(accounting.prepared == 0 && accounting.submitted == 4, "second submit drains prepared packets");
}

void test_submit_failure_keeps_packets_retryable() {
    wd_async_udp_accounting accounting{};
    wd_async_udp_accounting_queue(&accounting);
    wd_async_udp_accounting_queue(&accounting);

    require(wd_async_udp_accounting_submit_result(&accounting, -5) == 0, "negative submit moves no packets");
    require(accounting.prepared == 2 && accounting.submitted == 0, "failed submit leaves packets prepared");
    require(accounting.submit_failures == 1, "submit failure is counted separately");

    require(wd_async_udp_accounting_submit_result(&accounting, 2) == 2, "retry can submit retained packets");
    require(wd_async_udp_accounting_complete(&accounting, true), "successful completion accepted");
    require(wd_async_udp_accounting_complete(&accounting, false), "failed completion accepted");
    require(accounting.completed_total == 1 && accounting.failed_total == 1, "completion outcomes are separate");
}

void test_shutdown_cancels_only_unsubmitted_packets() {
    wd_async_udp_accounting accounting{};
    for (int i = 0; i < 3; ++i)
    {
        wd_async_udp_accounting_queue(&accounting);
    }
    require(wd_async_udp_accounting_submit_result(&accounting, 1) == 1, "one packet submitted");
    require(wd_async_udp_accounting_cancel_prepared(&accounting) == 2, "two prepared packets cancelled");
    require(accounting.submitted == 1 && accounting.cancelled_total == 2, "submitted packet remains in flight");
}

void test_pending_queue_limits_include_packet_and_byte_caps() {
    require(wd_async_udp_pending_within_limits(10, 1000, 100, 16, 2048), "queue below both limits should accept a packet");
    require(!wd_async_udp_pending_within_limits(16, 1000, 100, 16, 2048), "packet count cap should reject additional work");
    require(!wd_async_udp_pending_within_limits(10, 2000, 100, 16, 2048), "byte cap should reject additional work");
    require(!wd_async_udp_pending_within_limits(0, 0, 4096, 16, 2048), "single packet larger than byte cap should fail");
}

void test_stream_epoch_identity_rejects_stale_work() {
    const wd_stream_epoch_identity current{7, 3, 11, 19};
    require(wd_stream_epoch_identity_equal(&current, &current), "identical epochs match");

    wd_stream_epoch_identity stale = current;
    stale.connection_epoch--;
    require(!wd_stream_epoch_identity_equal(&current, &stale), "old connection is stale");
    stale = current;
    stale.config_epoch++;
    require(!wd_stream_epoch_identity_equal(&current, &stale), "different config is stale");
    stale = current;
    stale.content_epoch++;
    require(!wd_stream_epoch_identity_equal(&current, &stale), "different ownership is stale");
    stale = current;
    stale.framebuffer_generation++;
    require(!wd_stream_epoch_identity_equal(&current, &stale), "different framebuffer is stale");
}

} // namespace

int main() {
    test_partial_submit_is_not_counted_as_fully_submitted();
    test_submit_failure_keeps_packets_retryable();
    test_shutdown_cancels_only_unsubmitted_packets();
    test_pending_queue_limits_include_packet_and_byte_caps();
    test_stream_epoch_identity_rejects_stale_work();
    return 0;
}
