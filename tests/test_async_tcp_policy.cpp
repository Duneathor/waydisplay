#include "waydisplay/wd_async_tcp_policy.h"

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
    size_t sent = 0;
    CHECK(wd_async_tcp_advance(10, &sent, 4) == WD_ASYNC_TCP_SEND_PARTIAL && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, 0) == WD_ASYNC_TCP_SEND_FAILED && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, -1) == WD_ASYNC_TCP_SEND_FAILED && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, 7) == WD_ASYNC_TCP_SEND_FAILED && sent == 4);
    CHECK(wd_async_tcp_advance(10, &sent, 6) == WD_ASYNC_TCP_SEND_COMPLETE && sent == 10);
    CHECK(wd_async_tcp_advance(10, &sent, 1) == WD_ASYNC_TCP_SEND_FAILED && sent == 10);
    CHECK(wd_async_tcp_advance(10, nullptr, 1) == WD_ASYNC_TCP_SEND_FAILED);
    return 0;
}
