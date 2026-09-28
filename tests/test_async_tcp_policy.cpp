#include "waydisplay/wd_async_tcp_policy.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL: %s\n", #expr); std::exit(1); } } while (0)

int main() {
    CHECK(wd_async_tcp_can_enqueue(10, 10, 20));
    CHECK(!wd_async_tcp_can_enqueue(11, 10, 20));
    CHECK(wd_async_tcp_can_enqueue(0, 99, 0));
    CHECK(!wd_async_tcp_can_enqueue(UINT64_MAX, 1, 0));
    CHECK(!wd_async_tcp_can_enqueue(UINT64_MAX - 1, 3, UINT64_MAX));
    CHECK(!wd_async_tcp_can_enqueue(0, 21, 20));

    wd_async_tcp_owned_send_plan plan{};
    CHECK(wd_async_tcp_plan_owned_send(80, 257, 0, &plan));
    CHECK(plan.inline_offset == 0);
    CHECK(plan.inline_size == 80);
    CHECK(plan.payload_offset == 0);
    CHECK(plan.payload_size == 257);

    CHECK(wd_async_tcp_plan_owned_send(80, 257, 17, &plan));
    CHECK(plan.inline_offset == 17);
    CHECK(plan.inline_size == 63);
    CHECK(plan.payload_offset == 0);
    CHECK(plan.payload_size == 257);

    CHECK(wd_async_tcp_plan_owned_send(80, 257, 80, &plan));
    CHECK(plan.inline_size == 0);
    CHECK(plan.payload_offset == 0);
    CHECK(plan.payload_size == 257);

    CHECK(wd_async_tcp_plan_owned_send(80, 257, 101, &plan));
    CHECK(plan.inline_size == 0);
    CHECK(plan.payload_offset == 21);
    CHECK(plan.payload_size == 236);

    CHECK(!wd_async_tcp_plan_owned_send(80, 257, 337, &plan));

    /*
     * Exhaust every legal completion offset. A short send may end before,
     * exactly on, or after the inline-prefix/payload boundary; rebuilding the
     * next send plan must account for every already-sent byte exactly once.
     */
    constexpr size_t kInlineSize  = 80;
    constexpr size_t kPayloadSize = 257;
    constexpr size_t kTotalSize   = kInlineSize + kPayloadSize;
    for (size_t sent_bytes = 0; sent_bytes < kTotalSize; ++sent_bytes)
    {
        wd_async_tcp_owned_send_plan resumed{};
        CHECK(wd_async_tcp_plan_owned_send(kInlineSize, kPayloadSize, sent_bytes, &resumed));

        const size_t expected_inline_offset = sent_bytes < kInlineSize ? sent_bytes : 0;
        const size_t expected_inline_size =
            sent_bytes < kInlineSize ? kInlineSize - sent_bytes : 0;
        const size_t expected_payload_offset =
            sent_bytes > kInlineSize ? sent_bytes - kInlineSize : 0;
        const size_t expected_payload_size = kPayloadSize - expected_payload_offset;

        CHECK(resumed.inline_offset == expected_inline_offset);
        CHECK(resumed.inline_size == expected_inline_size);
        CHECK(resumed.payload_offset == expected_payload_offset);
        CHECK(resumed.payload_size == expected_payload_size);
        CHECK(resumed.inline_size + resumed.payload_size == kTotalSize - sent_bytes);
    }

    CHECK(!wd_async_tcp_plan_owned_send(
        std::numeric_limits<size_t>::max(), 1, 0, &plan));
    CHECK(!wd_async_tcp_plan_owned_send(80, 257, 0, nullptr));

    size_t sent = 0;
    CHECK(wd_async_tcp_advance(10, &sent, 4) == WD_ASYNC_TCP_SEND_PARTIAL && sent == 4);
    /* io_uring may surface transient send errors in a CQE. They must leave
     * the retained message offset untouched so the sender can resubmit it
     * instead of poisoning the whole reconnecting session. */
    CHECK(wd_async_tcp_advance(10, &sent, -EINTR) == WD_ASYNC_TCP_SEND_PARTIAL && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, -EAGAIN) == WD_ASYNC_TCP_SEND_PARTIAL && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, -EPIPE) == WD_ASYNC_TCP_SEND_FAILED && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, 0) == WD_ASYNC_TCP_SEND_FAILED && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, 7) == WD_ASYNC_TCP_SEND_FAILED && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, 6) == WD_ASYNC_TCP_SEND_COMPLETE && sent == 10);
    CHECK(wd_async_tcp_advance(10, &sent, 1) == WD_ASYNC_TCP_SEND_FAILED && sent == 10);
    CHECK(wd_async_tcp_advance(10, nullptr, 1) == WD_ASYNC_TCP_SEND_FAILED);
    return 0;
}
