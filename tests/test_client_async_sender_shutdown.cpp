#include "client_async_tcp.hpp"
#include "waydisplay/wd_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t TestVideoBytes = 512u * 1024u;
}

#define CHECK(condition)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(condition))                                                                                                                  \
        {                                                                                                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                     \
            std::exit(1);                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

namespace {

void move_socket_endpoint_to_fd(int source_fd, int& peer_fd, int target_fd) {
    if (source_fd == target_fd)
    {
        return;
    }

    if (peer_fd == target_fd)
    {
        const int moved_peer = ::dup(peer_fd);
        CHECK(moved_peer >= 0);
        CHECK(::fcntl(moved_peer, F_SETFD, FD_CLOEXEC) == 0);
        ::close(peer_fd);
        peer_fd = moved_peer;
    }

    CHECK(::dup2(source_fd, target_fd) == target_fd);
    CHECK(::fcntl(target_fd, F_SETFD, FD_CLOEXEC) == 0);
}

int test_forced_shutdown() {
    using namespace waydisplay;

    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(8, 2u * 1024u * 1024u);
    if (!sender)
    {
        return 77;
    }

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    int send_buffer_size = 4096;
    CHECK(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) == 0);

    const unsigned char malformed_payload[32] = {};
    CHECK(!client_async_tcp_send_message(sender, sockets[0], WD_MSG_POINTER_EVENT, malformed_payload,
                                         sizeof(malformed_payload)));

    const uint32_t payload_size = static_cast<uint32_t>(sizeof(wd_video_frame_payload_header)) + TestVideoBytes;
    std::vector<uint8_t> payload(payload_size);
    auto* header      = reinterpret_cast<wd_video_frame_payload_header*>(payload.data());
    header->data_size = TestVideoBytes;
    CHECK(client_async_tcp_send_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, payload.data(), payload_size));

    const ClientAsyncTcpSenderStats stats = client_async_tcp_sender_destroy(sender);
    CHECK(stats.queued == 1);
    CHECK(stats.completed == 0);
    CHECK(stats.failed == 2);
    CHECK(stats.inflight == 0);
    CHECK(stats.pending_bytes == 0);

    ::close(sockets[0]);
    ::close(sockets[1]);
    return 0;
}

int test_teardown_does_not_hit_reused_fd() {
    using namespace waydisplay;

    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(8, 2u * 1024u * 1024u);
    if (!sender)
    {
        return 77;
    }

    int old_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, old_pair) == 0);
    int send_buffer_size = 4096;
    CHECK(::setsockopt(old_pair[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) == 0);

    const uint32_t payload_size = static_cast<uint32_t>(sizeof(wd_video_frame_payload_header)) + TestVideoBytes;
    std::vector<uint8_t> payload(payload_size);
    reinterpret_cast<wd_video_frame_payload_header*>(payload.data())->data_size = TestVideoBytes;
    CHECK(client_async_tcp_send_message(sender, old_pair[0], WD_MSG_VIDEO_FRAME, payload.data(), payload_size));

    const int recycled_fd = old_pair[0];
    ::close(old_pair[0]);
    old_pair[0] = -1;

    int replacement[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, replacement) == 0);
    if (replacement[0] != recycled_fd)
    {
        move_socket_endpoint_to_fd(replacement[0], replacement[1], recycled_fd);
        ::close(replacement[0]);
        replacement[0] = recycled_fd;
    }

    (void)client_async_tcp_sender_destroy(sender);

    const uint8_t byte = 0x3c;
    uint8_t received = 0;
    CHECK(::send(replacement[0], &byte, 1, MSG_NOSIGNAL) == 1);
    CHECK(::recv(replacement[1], &received, 1, MSG_WAITALL) == 1);
    CHECK(received == byte);

    ::close(old_pair[1]);
    ::close(replacement[0]);
    ::close(replacement[1]);
    return 0;
}

int test_pointer_motion_coalesces_before_capacity_rejection() {
    using namespace waydisplay;

    const uint32_t video_payload_size = static_cast<uint32_t>(sizeof(wd_video_frame_payload_header)) + TestVideoBytes;
    const uint64_t video_wire_size = WD_TCP_HEADER_WIRE_SIZE + static_cast<uint64_t>(video_payload_size);
    const uint64_t pointer_wire_size = WD_TCP_HEADER_WIRE_SIZE + sizeof(wd_pointer_event_payload);
    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(8, video_wire_size + pointer_wire_size);
    if (!sender)
    {
        return 77;
    }

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int send_buffer_size = 4096;
    CHECK(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) == 0);

    std::vector<uint8_t> video(video_payload_size);
    auto* video_header      = reinterpret_cast<wd_video_frame_payload_header*>(video.data());
    video_header->data_size = TestVideoBytes;
    CHECK(client_async_tcp_send_message(sender, sockets[0], WD_MSG_VIDEO_FRAME, video.data(), video_payload_size));

    wd_pointer_event_payload first{};
    first.session_id = 1;
    first.connection_token = 1;
    first.client_timestamp_ns = 1;
    first.input_sequence = 1;
    first.event_type = WD_POINTER_EVENT_MOTION;
    first.x = 10;
    first.y = 20;
    CHECK(client_async_tcp_send_message(sender, sockets[0], WD_MSG_POINTER_EVENT, &first, sizeof(first)));

    wd_pointer_event_payload replacement = first;
    replacement.client_timestamp_ns = 2;
    replacement.input_sequence = 2;
    replacement.x = 30;
    replacement.y = 40;
    CHECK(client_async_tcp_send_message(sender, sockets[0], WD_MSG_POINTER_EVENT, &replacement, sizeof(replacement)));

    const ClientAsyncTcpSenderStats live = client_async_tcp_sender_stats(sender);
    CHECK(live.coalesced == 1);
    CHECK(live.overflows == 0);
    CHECK(live.pending_bytes <= video_wire_size + pointer_wire_size);

    (void)client_async_tcp_sender_destroy(sender);
    ::close(sockets[0]);
    ::close(sockets[1]);
    return 0;
}

} // namespace

int main() {
    int result = test_forced_shutdown();
    if (result != 0)
    {
        return result;
    }
    result = test_teardown_does_not_hit_reused_fd();
    if (result != 0)
    {
        return result;
    }
    return test_pointer_motion_coalesces_before_capacity_rejection();
}
