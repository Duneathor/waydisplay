#include "waydisplay/wd_buffer.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_codec.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

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

std::array<uint8_t, WD_TCP_HEADER_WIRE_SIZE> make_header(uint16_t type, uint32_t payload_size) {
    wd_tcp_header header{};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = type;
    header.payload_size     = payload_size;

    std::array<uint8_t, WD_TCP_HEADER_WIRE_SIZE> wire{};
    CHECK(wd_tcp_header_encode(wire.data(), &header));
    return wire;
}

void send_exact(int fd, const void* bytes, size_t size) {
    const auto* data = static_cast<const uint8_t*>(bytes);
    while (size != 0)
    {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        CHECK(sent > 0);
        data += static_cast<size_t>(sent);
        size -= static_cast<size_t>(sent);
    }
}

void test_fragmented_receive_transfers_owner() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    constexpr uint32_t PayloadSize = static_cast<uint32_t>(sizeof(wd_video_frame_payload_header)) + 8192u;
    std::vector<uint8_t> payload(PayloadSize);
    for (uint32_t i = 0; i < PayloadSize; ++i)
    {
        payload[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xffu);
    }

    const auto wire = make_header(WD_MSG_VIDEO_FRAME, PayloadSize);
    wd_tcp_reader reader{};
    wd_tcp_reader_init(&reader, PayloadSize);

    send_exact(sockets[0], wire.data(), 5);
    wd_tcp_message message{};
    CHECK(wd_tcp_reader_receive(&reader, sockets[1], 100, 1000, 10000, &message) == WD_TCP_READER_NEED_MORE);
    CHECK(message.buffer == nullptr && message.payload == nullptr);
    CHECK(wd_tcp_reader_has_partial_frame(&reader));

    send_exact(sockets[0], wire.data() + 5, wire.size() - 5);
    send_exact(sockets[0], payload.data(), 777);
    CHECK(wd_tcp_reader_receive(&reader, sockets[1], 200, 1000, 10000, &message) == WD_TCP_READER_NEED_MORE);
    CHECK(message.buffer == nullptr && message.payload == nullptr);

    send_exact(sockets[0], payload.data() + 777, payload.size() - 777);
    CHECK(wd_tcp_reader_receive(&reader, sockets[1], 300, 1000, 10000, &message) == WD_TCP_READER_MESSAGE);
    CHECK(message.message_type == WD_MSG_VIDEO_FRAME);
    CHECK(message.buffer != nullptr);
    CHECK(message.payload == wd_buffer_data(message.buffer));
    CHECK(message.payload_size == PayloadSize);
    CHECK(wd_buffer_size(message.buffer) == PayloadSize);
    CHECK(wd_buffer_capacity(message.buffer) >= PayloadSize + WD_TCP_PAYLOAD_PADDING_BYTES);
    CHECK(std::memcmp(message.payload, payload.data(), payload.size()) == 0);
    for (size_t i = PayloadSize; i < PayloadSize + WD_TCP_PAYLOAD_PADDING_BYTES; ++i)
    {
        CHECK(wd_buffer_data(message.buffer)[i] == 0);
    }
    CHECK(!wd_tcp_reader_has_partial_frame(&reader));

    wd_buffer* owner = wd_tcp_message_take_buffer(&message);
    CHECK(owner != nullptr);
    CHECK(message.buffer == nullptr);
    CHECK(message.payload == nullptr);
    CHECK(message.payload_size == 0);
    wd_tcp_message_release(&message);

    CHECK(wd_buffer_size(owner) == PayloadSize);
    CHECK(std::memcmp(wd_buffer_const_data(owner), payload.data(), payload.size()) == 0);
    wd_buffer* retained = wd_buffer_retain(owner);
    CHECK(retained == owner);
    wd_buffer_release(owner);
    CHECK(std::memcmp(wd_buffer_const_data(retained), payload.data(), payload.size()) == 0);
    wd_buffer_release(retained);

    wd_tcp_reader_destroy(&reader);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_message_release_and_reader_reset() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    const std::array<uint8_t, sizeof(wd_link_probe_payload)> payload{};
    const auto wire = make_header(WD_MSG_LINK_PROBE_PING, payload.size());
    send_exact(sockets[0], wire.data(), wire.size());
    send_exact(sockets[0], payload.data(), payload.size());

    wd_tcp_reader reader{};
    wd_tcp_reader_init(&reader, 64);
    wd_tcp_message message{};
    CHECK(wd_tcp_reader_receive(&reader, sockets[1], 100, 1000, 10000, &message) == WD_TCP_READER_MESSAGE);
    CHECK(message.buffer != nullptr);
    wd_tcp_message_release(&message);
    CHECK(message.buffer == nullptr && message.payload == nullptr && message.payload_size == 0);

    /* Destroy/reset must be safe while a newly allocated payload is partial. */
    const auto partial_wire = make_header(WD_MSG_VIDEO_FRAME, 64);
    send_exact(sockets[0], partial_wire.data(), partial_wire.size());
    const uint8_t partial[17] = {};
    send_exact(sockets[0], partial, sizeof(partial));
    CHECK(wd_tcp_reader_receive(&reader, sockets[1], 200, 1000, 10000, &message) == WD_TCP_READER_NEED_MORE);
    CHECK(reader.payload_buffer != nullptr);
    wd_tcp_reader_reset(&reader);
    CHECK(reader.payload_buffer == nullptr);
    CHECK(!wd_tcp_reader_has_partial_frame(&reader));

    wd_tcp_reader_destroy(&reader);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_zero_payload_message() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    const auto wire = make_header(WD_MSG_CLIPBOARD_REQUEST, 0);
    send_exact(sockets[0], wire.data(), wire.size());

    wd_tcp_reader reader{};
    wd_tcp_reader_init(&reader, 64);
    wd_tcp_message message{};
    CHECK(wd_tcp_reader_receive(&reader, sockets[1], 100, 1000, 10000, &message) == WD_TCP_READER_MESSAGE);
    CHECK(message.message_type == WD_MSG_CLIPBOARD_REQUEST);
    CHECK(message.buffer == nullptr);
    CHECK(message.payload == nullptr);
    CHECK(message.payload_size == 0);
    CHECK(wd_tcp_message_take_buffer(&message) == nullptr);
    wd_tcp_message_release(&message);

    wd_tcp_reader_destroy(&reader);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

} // namespace

int main() {
    test_fragmented_receive_transfers_owner();
    test_message_release_and_reader_reset();
    test_zero_payload_message();
    return 0;
}
