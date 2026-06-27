#include "client_session.h"
#include "stream_ownership.h"
#include "wd_async_udp_accounting.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                    \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

void test_shutdown_unblocks_every_channel_and_reconnects() {
    wd_client_session session{};
    wd_client_session_init(&session);
    CHECK(wd_client_session_begin_connect(&session));

    std::array<std::array<int, 2>, 6> pairs{};
    for (auto& pair : pairs)
    {
        pair = {-1, -1};
        CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0);
    }
    session.control_fd   = pairs[0][0];
    session.input_fd     = pairs[1][0];
    session.selection_fd = pairs[2][0];
    session.video_fd     = pairs[3][0];
    session.audio_fd     = pairs[4][0];
    session.udp_fd       = pairs[5][0];
    wd_client_session_mark_connected(&session);
    CHECK(session.phase == WD_CLIENT_SESSION_CONNECTED);

    std::atomic<unsigned> exited{0};
    std::vector<std::thread> readers;
    for (const auto& pair : pairs)
    {
        readers.emplace_back([fd = pair[0], &exited]() {
            char byte = 0;
            for (;;)
            {
                const ssize_t count = recv(fd, &byte, 1, 0);
                if (count == 0)
                {
                    break;
                }
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count < 0)
                {
                    CHECK(errno == ECONNRESET || errno == ENOTCONN || errno == EBADF);
                    break;
                }
            }
            exited.fetch_add(1, std::memory_order_release);
        });
    }

    CHECK(wd_client_session_begin_shutdown(&session));
    CHECK(!wd_client_session_begin_shutdown(&session));
    wd_client_session_shutdown_open_fds(&session);
    for (auto& reader : readers)
    {
        reader.join();
    }
    CHECK(exited.load(std::memory_order_acquire) == pairs.size());

    wd_client_session_close_open_fds(&session);
    CHECK(wd_client_session_fds_closed(&session));
    CHECK(session.phase == WD_CLIENT_SESSION_IDLE);
    for (auto& pair : pairs)
    {
        close(pair[1]);
        pair[1] = -1;
    }

    /* A clean session object must allow immediate reconnect even when the OS
     * recycles the same descriptor numbers. */
    CHECK(wd_client_session_begin_connect(&session));
    int replacement[2]{-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, replacement) == 0);
    session.control_fd = replacement[0];
    wd_client_session_mark_connected(&session);
    CHECK(session.phase == WD_CLIENT_SESSION_CONNECTED);
    CHECK(wd_client_session_begin_shutdown(&session));
    wd_client_session_shutdown_open_fds(&session);
    wd_client_session_close_open_fds(&session);
    close(replacement[1]);
}

void test_stale_async_identity_is_rejected_after_rotation() {
    const wd_stream_epoch_identity session_a{1, 1, 1, 1};
    const wd_stream_epoch_identity session_b{2, 1, 1, 1};
    CHECK(wd_stream_epoch_identity_equal(&session_a, &session_a));
    CHECK(!wd_stream_epoch_identity_equal(&session_a, &session_b));

    std::atomic<uint64_t> current_connection{session_a.connection_epoch};
    std::atomic<uint64_t> accepted_a{0};
    std::atomic<uint64_t> accepted_b{0};
    constexpr uint64_t Iterations = 100000;

    std::thread completion([&]() {
        for (uint64_t i = 0; i < Iterations; ++i)
        {
            const wd_stream_epoch_identity work = (i & 1u) == 0 ? session_a : session_b;
            const wd_stream_epoch_identity current{current_connection.load(std::memory_order_acquire), 1, 1, 1};
            if (wd_stream_epoch_identity_equal(&work, &current))
            {
                (work.connection_epoch == 1 ? accepted_a : accepted_b).fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    current_connection.store(session_b.connection_epoch, std::memory_order_release);
    completion.join();
    CHECK(accepted_b.load(std::memory_order_relaxed) != 0);
    /* Depending on scheduling, A may complete before rotation. It must never
     * be accepted once a B snapshot has been observed; exact identity equality
     * is the gate that enforces that property. */
    const wd_stream_epoch_identity current{current_connection.load(std::memory_order_acquire), 1, 1, 1};
    CHECK(!wd_stream_epoch_identity_equal(&session_a, &current));
    CHECK(wd_stream_epoch_identity_equal(&session_b, &current));
}

void test_stream_ownership_is_atomic_under_contention() {
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;
    const uint64_t initial_epoch = wd_client_stream_ownership_snapshot(&ownership).epoch;
    constexpr unsigned ThreadCount = 4;
    constexpr unsigned PerThread = 20000;
    std::vector<std::thread> workers;
    for (unsigned thread_index = 0; thread_index < ThreadCount; ++thread_index)
    {
        workers.emplace_back([thread_index, &ownership]() {
            for (unsigned i = 0; i < PerThread; ++i)
            {
                if (((i + thread_index) & 1u) == 0)
                {
                    wd_client_stream_ownership_reset_to_video(&ownership);
                }
                else
                {
                    wd_client_stream_ownership_reset_to_tiles(&ownership);
                }
            }
        });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    const auto snapshot = wd_client_stream_ownership_snapshot(&ownership);
    CHECK(snapshot.epoch == initial_epoch + static_cast<uint64_t>(ThreadCount) * PerThread);
    CHECK(wd_client_stream_ownership_is_current(&ownership, snapshot.epoch, snapshot.owner));
    CHECK(!wd_client_stream_ownership_is_current(&ownership, snapshot.epoch - 1, snapshot.owner));
}

void test_accounting_shutdown_does_not_complete_twice() {
    wd_async_udp_accounting accounting{};
    for (unsigned i = 0; i < 8; ++i)
    {
        wd_async_udp_accounting_queue(&accounting);
    }
    CHECK(wd_async_udp_accounting_submit_result(&accounting, 3) == 3);
    CHECK(wd_async_udp_accounting_cancel_prepared(&accounting) == 5);
    for (unsigned i = 0; i < 3; ++i)
    {
        CHECK(wd_async_udp_accounting_complete(&accounting, (i & 1u) == 0));
    }
    CHECK(!wd_async_udp_accounting_complete(&accounting, true));
    CHECK(wd_async_udp_accounting_pending(&accounting) == 0);
    CHECK(accounting.completed_total == 2 && accounting.failed_total == 1 && accounting.cancelled_total == 5);
}

} // namespace

int main() {
    test_shutdown_unblocks_every_channel_and_reconnects();
    test_stale_async_identity_is_rejected_after_rotation();
    test_stream_ownership_is_atomic_under_contention();
    test_accounting_shutdown_does_not_complete_twice();
    return 0;
}
