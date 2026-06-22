#include "client_async_udp.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"

#include <liburing.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <unistd.h>
#include <utility>
#include <vector>

namespace waydisplay {
namespace {


char g_cancel_cqe_tag;
void* const CANCEL_CQE = &g_cancel_cqe_tag;

struct Buffer {
    std::vector<uint8_t> bytes;
    bool                 prepared        = false;
    bool                 submitted       = false;
    bool                 cancel_requested = false;
};

bool set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    if ((flags & O_NONBLOCK) == 0)
    {
        return true;
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

bool is_transient_receive_error(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECANCELED;
}

} // namespace

struct ClientAsyncUdpReceiver {
    std::mutex mutex;
    io_uring  ring{};
    bool      ring_ready = false;
    int       fd         = -1;

    std::vector<Buffer>  buffers;
    std::vector<Buffer*> prepared;

    uint64_t inflight      = 0;
    uint64_t inflight_max  = 0;
    uint64_t submitted     = 0;
    uint64_t retired       = 0;
    uint64_t completed     = 0;
    uint64_t failed        = 0;
    uint64_t submit_failed = 0;
    uint64_t cancels       = 0;
    uint64_t accounting_errors = 0;
    bool     fatal         = false;
};

namespace {

void clear_prepared_locked(ClientAsyncUdpReceiver* receiver) {
    if (!receiver)
    {
        return;
    }
    for (Buffer* buffer : receiver->prepared)
    {
        if (buffer)
        {
            buffer->prepared = false;
        }
    }
    receiver->prepared.clear();
}

bool prepare_receive_locked(ClientAsyncUdpReceiver* receiver, Buffer& buffer) {
    if (!receiver || !receiver->ring_ready || receiver->fd < 0 || buffer.prepared || buffer.submitted ||
        buffer.bytes.empty())
    {
        return false;
    }

    io_uring_sqe* sqe = io_uring_get_sqe(&receiver->ring);
    if (!sqe)
    {
        return false;
    }

    io_uring_prep_recv(sqe, receiver->fd, buffer.bytes.data(), buffer.bytes.size(), 0);
    io_uring_sqe_set_data(sqe, &buffer);

    buffer.prepared = true;
    receiver->prepared.push_back(&buffer);
    return true;
}

bool flush_prepared_locked(ClientAsyncUdpReceiver* receiver) {
    if (!receiver || !receiver->ring_ready || receiver->fatal || receiver->prepared.empty())
    {
        return receiver && !receiver->fatal;
    }

    const size_t prepared_count = receiver->prepared.size();
    const int    rc             = io_uring_submit(&receiver->ring);
    if (rc < 0)
    {
        receiver->submit_failed += prepared_count;
        receiver->fatal = true;
        clear_prepared_locked(receiver);
        return false;
    }

    if (rc == 0)
    {
        receiver->submit_failed += prepared_count;
        receiver->fatal = true;
        clear_prepared_locked(receiver);
        return false;
    }

    const size_t submitted_now = std::min<size_t>(static_cast<size_t>(rc), prepared_count);
    for (size_t i = 0; i < submitted_now; ++i)
    {
        Buffer* buffer = receiver->prepared[i];
        if (!buffer || !buffer->prepared)
        {
            continue;
        }
        buffer->prepared  = false;
        buffer->submitted = true;
        receiver->inflight++;
        receiver->submitted++;
        if (receiver->inflight > receiver->inflight_max)
        {
            receiver->inflight_max = receiver->inflight;
        }
    }

    if (submitted_now != 0)
    {
        receiver->prepared.erase(receiver->prepared.begin(), receiver->prepared.begin() +
                                                        static_cast<std::ptrdiff_t>(submitted_now));
    }
    return true;
}

bool post_receives_locked(ClientAsyncUdpReceiver* receiver) {
    if (!receiver || !receiver->ring_ready || receiver->fatal)
    {
        return false;
    }

    if (!receiver->prepared.empty())
    {
        return flush_prepared_locked(receiver);
    }

    for (Buffer& buffer : receiver->buffers)
    {
        if (buffer.prepared || buffer.submitted)
        {
            continue;
        }
        if (!prepare_receive_locked(receiver, buffer))
        {
            break;
        }
    }

    return flush_prepared_locked(receiver);
}


bool submit_prepared_for_shutdown_locked(ClientAsyncUdpReceiver* receiver) {
    if (!receiver || !receiver->ring_ready || receiver->prepared.empty())
    {
        return receiver && receiver->prepared.empty();
    }

    const size_t prepared_count = receiver->prepared.size();
    const int rc = io_uring_submit(&receiver->ring);
    if (rc <= 0)
    {
        return false;
    }

    const size_t submitted_now = std::min<size_t>(static_cast<size_t>(rc), prepared_count);
    for (size_t i = 0; i < submitted_now; ++i)
    {
        Buffer* buffer = receiver->prepared[i];
        if (!buffer || !buffer->prepared)
        {
            continue;
        }
        buffer->prepared = false;
        buffer->submitted = true;
        receiver->inflight++;
        receiver->submitted++;
        receiver->inflight_max = std::max(receiver->inflight_max, receiver->inflight);
    }
    receiver->prepared.erase(receiver->prepared.begin(), receiver->prepared.begin() +
                                                     static_cast<std::ptrdiff_t>(submitted_now));
    return receiver->prepared.empty();
}

bool has_submitted_locked(const ClientAsyncUdpReceiver* receiver) {
    if (!receiver)
    {
        return false;
    }
    for (const Buffer& buffer : receiver->buffers)
    {
        if (buffer.submitted)
        {
            return true;
        }
    }
    return false;
}

void check_accounting_locked(ClientAsyncUdpReceiver* receiver) {
    if (receiver && receiver->submitted != receiver->retired + receiver->inflight)
    {
        receiver->accounting_errors++;
    }
}

void mark_buffer_complete_locked(ClientAsyncUdpReceiver* receiver, Buffer* buffer) {
    if (buffer && buffer->submitted)
    {
        buffer->submitted = false;
        buffer->cancel_requested = false;
        if (receiver)
        {
            receiver->retired++;
            if (receiver->inflight > 0)
            {
                receiver->inflight--;
            }
            else
            {
                receiver->accounting_errors++;
            }
            check_accounting_locked(receiver);
        }
    }
}

void request_cancels_locked(ClientAsyncUdpReceiver* receiver) {
    if (!receiver || !receiver->ring_ready)
    {
        return;
    }

    std::vector<Buffer*> requested;
    requested.reserve(receiver->buffers.size());
    for (Buffer& buffer : receiver->buffers)
    {
        if (!buffer.submitted || buffer.cancel_requested)
        {
            continue;
        }
        io_uring_sqe* sqe = io_uring_get_sqe(&receiver->ring);
        if (!sqe)
        {
            break;
        }
        io_uring_prep_cancel(sqe, &buffer, 0);
        io_uring_sqe_set_data(sqe, CANCEL_CQE);
        buffer.cancel_requested = true;
        requested.push_back(&buffer);
    }

    if (requested.empty())
    {
        return;
    }

    const int submitted = io_uring_submit(&receiver->ring);
    if (submitted <= 0)
    {
        for (Buffer* buffer : requested)
        {
            if (buffer)
            {
                buffer->cancel_requested = false;
            }
        }
    }
}

bool drain_destroy_locked(ClientAsyncUdpReceiver* receiver) {
    if (!receiver || !receiver->ring_ready)
    {
        return true;
    }

    for (uint32_t i = 0; (!receiver->prepared.empty() || has_submitted_locked(receiver)) && i < WD_ASYNC_SENDER_DRAIN_LIMIT; ++i)
    {
        if (!receiver->prepared.empty())
        {
            (void)submit_prepared_for_shutdown_locked(receiver);
        }
        if (receiver->prepared.empty())
        {
            request_cancels_locked(receiver);
        }

        io_uring_cqe* cqe = nullptr;
        while (io_uring_peek_cqe(&receiver->ring, &cqe) == 0 && cqe)
        {
            void* data = io_uring_cqe_get_data(cqe);
            if (data == CANCEL_CQE)
            {
                receiver->cancels++;
            }
            else
            {
                auto* buffer = static_cast<Buffer*>(data);
                mark_buffer_complete_locked(receiver, buffer);
            }
            io_uring_cqe_seen(&receiver->ring, cqe);
            cqe = nullptr;
        }

        if (receiver->prepared.empty() && !has_submitted_locked(receiver))
        {
            break;
        }
        usleep(WD_ASYNC_SENDER_DRAIN_SLEEP_US);
    }

    return receiver->prepared.empty() && !has_submitted_locked(receiver);
}

} // namespace

ClientAsyncUdpReceiver* client_async_udp_receiver_create(int fd, uint32_t entries, size_t packet_size) {
    if (fd < 0)
    {
        return nullptr;
    }
    if (entries == 0)
    {
        entries = WD_CLIENT_ASYNC_UDP_DEFAULT_RING_ENTRIES;
    }
    if (entries < WD_ASYNC_MIN_RING_ENTRIES)
    {
        entries = WD_ASYNC_MIN_RING_ENTRIES;
    }
    if (packet_size == 0)
    {
        packet_size = WD_CLIENT_ASYNC_UDP_DEFAULT_PACKET_BYTES;
    }

    auto* receiver = new (std::nothrow) ClientAsyncUdpReceiver();
    if (!receiver)
    {
        return nullptr;
    }

    receiver->fd = fd;
    receiver->buffers.resize(entries);
    receiver->prepared.reserve(entries);
    for (Buffer& buffer : receiver->buffers)
    {
        buffer.bytes.resize(packet_size);
    }

    const int rc = io_uring_queue_init(entries, &receiver->ring, 0);
    if (rc < 0)
    {
        delete receiver;
        return nullptr;
    }

    receiver->ring_ready = true;

    if (!set_blocking(fd))
    {
        io_uring_queue_exit(&receiver->ring);
        delete receiver;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        if (!post_receives_locked(receiver))
        {
            if (!drain_destroy_locked(receiver))
            {
                WD_LOG_ERROR("client UDP io_uring setup timed out during drain; receiver still owns the socket");
                return receiver;
            }
            io_uring_queue_exit(&receiver->ring);
            receiver->ring_ready = false;
            delete receiver;
            return nullptr;
        }
    }

    return receiver;
}

ClientAsyncUdpDetachResult client_async_udp_receiver_destroy(ClientAsyncUdpReceiver* receiver,
                                                               ClientAsyncUdpReceiverStats* final_stats) {
    if (!receiver)
    {
        return ClientAsyncUdpDetachResult::Detached;
    }

    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        if (!drain_destroy_locked(receiver))
        {
            if (final_stats)
            {
                final_stats->posted            = receiver->submitted;
                final_stats->retired           = receiver->retired;
                final_stats->completed         = receiver->completed;
                final_stats->failed            = receiver->failed;
                final_stats->submit_failed     = receiver->submit_failed;
                final_stats->cancels           = receiver->cancels;
                final_stats->inflight          = receiver->inflight;
                final_stats->prepared          = receiver->prepared.size();
                final_stats->inflight_max      = receiver->inflight_max;
                final_stats->accounting_errors = receiver->accounting_errors;
            }
            WD_LOG_ERROR("client UDP io_uring detach timed out; outstanding receives still own the socket");
            return ClientAsyncUdpDetachResult::SocketStillOwned;
        }
        if (final_stats)
        {
            final_stats->posted            = receiver->submitted;
            final_stats->retired           = receiver->retired;
            final_stats->completed         = receiver->completed;
            final_stats->failed            = receiver->failed;
            final_stats->submit_failed     = receiver->submit_failed;
            final_stats->cancels           = receiver->cancels;
            final_stats->inflight          = receiver->inflight;
            final_stats->prepared          = receiver->prepared.size();
            final_stats->inflight_max      = receiver->inflight_max;
            final_stats->accounting_errors = receiver->accounting_errors;
        }
        if (receiver->ring_ready)
        {
            io_uring_queue_exit(&receiver->ring);
            receiver->ring_ready = false;
        }
    }

    delete receiver;
    return ClientAsyncUdpDetachResult::Detached;
}

bool client_async_udp_receiver_ready(ClientAsyncUdpReceiver* receiver) {
    if (!receiver)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(receiver->mutex);
    return receiver->ring_ready && !receiver->fatal;
}

ClientAsyncUdpWaitResult client_async_udp_receiver_wait(ClientAsyncUdpReceiver* receiver, uint64_t timeout_ns) {
    if (!receiver)
    {
        return ClientAsyncUdpWaitResult::Failed;
    }

    std::lock_guard<std::mutex> lock(receiver->mutex);
    if (!receiver->ring_ready || receiver->fatal || !post_receives_locked(receiver))
    {
        return ClientAsyncUdpWaitResult::Failed;
    }

    __kernel_timespec timeout{};
    timeout.tv_sec = static_cast<__kernel_time64_t>(timeout_ns / WD_NSEC_PER_SEC);
    timeout.tv_nsec = static_cast<long>(timeout_ns % WD_NSEC_PER_SEC);

    io_uring_cqe* cqe = nullptr;
    const int rc = io_uring_wait_cqe_timeout(&receiver->ring, &cqe, &timeout);
    if (rc == 0 && cqe)
    {
        return ClientAsyncUdpWaitResult::Ready;
    }
    if (rc == -ETIME || rc == -EAGAIN || rc == -EINTR)
    {
        return ClientAsyncUdpWaitResult::Timeout;
    }

    receiver->failed++;
    receiver->fatal = true;
    return ClientAsyncUdpWaitResult::Failed;
}

bool client_async_udp_receiver_drain(ClientAsyncUdpReceiver* receiver, void* userdata,
                                     ClientAsyncUdpPacketHandler handler, uint32_t max_packets) {
    if (!receiver || !handler)
    {
        return false;
    }

    if (max_packets == 0)
    {
        max_packets = WD_CLIENT_ASYNC_UDP_DEFAULT_DRAIN_BATCH;
    }

    std::vector<std::pair<Buffer*, int>> completed;
    completed.reserve(std::min<uint32_t>(max_packets, WD_CLIENT_ASYNC_UDP_COMPLETION_RESERVE));

    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        if (!post_receives_locked(receiver))
        {
            return false;
        }

        while (completed.size() < max_packets)
        {
            io_uring_cqe* cqe = nullptr;
            if (io_uring_peek_cqe(&receiver->ring, &cqe) != 0 || !cqe)
            {
                break;
            }

            void* data = io_uring_cqe_get_data(cqe);
            if (data == CANCEL_CQE)
            {
                receiver->cancels++;
                io_uring_cqe_seen(&receiver->ring, cqe);
                continue;
            }

            auto* buffer = static_cast<Buffer*>(data);
            mark_buffer_complete_locked(receiver, buffer);

            if (cqe->res > 0)
            {
                receiver->completed++;
                completed.emplace_back(buffer, cqe->res);
            }
            else if (cqe->res < 0)
            {
                const int err = -cqe->res;
                if (err == ECANCELED)
                {
                    receiver->cancels++;
                }
                else if (!is_transient_receive_error(err))
                {
                    receiver->failed++;
                    receiver->fatal = true;
                }
            }

            io_uring_cqe_seen(&receiver->ring, cqe);
        }
    }

    for (const auto& packet : completed)
    {
        Buffer* buffer = packet.first;
        const int size = packet.second;
        if (buffer && size > 0 && !handler(userdata, buffer->bytes.data(), static_cast<size_t>(size)))
        {
            std::lock_guard<std::mutex> lock(receiver->mutex);
            receiver->failed++;
            receiver->fatal = true;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        if (!post_receives_locked(receiver))
        {
            return false;
        }
        return !receiver->fatal;
    }
}

ClientAsyncUdpReceiverStats client_async_udp_receiver_stats(ClientAsyncUdpReceiver* receiver) {
    ClientAsyncUdpReceiverStats stats{};
    if (!receiver)
    {
        return stats;
    }

    std::lock_guard<std::mutex> lock(receiver->mutex);
    stats.posted            = receiver->submitted;
    stats.retired           = receiver->retired;
    stats.completed         = receiver->completed;
    stats.failed            = receiver->failed;
    stats.submit_failed     = receiver->submit_failed;
    stats.cancels           = receiver->cancels;
    stats.inflight          = receiver->inflight;
    stats.prepared          = receiver->prepared.size();
    stats.inflight_max      = receiver->inflight_max;
    stats.accounting_errors = receiver->accounting_errors;
    return stats;
}

} // namespace waydisplay
