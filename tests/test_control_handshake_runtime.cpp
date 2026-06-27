#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_codec.h"
#include "waydisplay/wd_protocol_dispatch.h"
#include "wd_channel_binding.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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

struct SocketPair {
    int fd[2]{-1, -1};
    SocketPair() {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) == 0);
        const int flags = fcntl(fd[1], F_GETFL, 0);
        CHECK(flags >= 0 && fcntl(fd[1], F_SETFL, flags | O_NONBLOCK) == 0);
    }
    ~SocketPair() {
        for (int& value : fd)
        {
            if (value >= 0)
            {
                close(value);
                value = -1;
            }
        }
    }
};

std::array<uint8_t, WD_TCP_HEADER_WIRE_SIZE> encode_header(uint16_t type, uint32_t payload_size, uint32_t magic = WD_TCP_MAGIC) {
    wd_tcp_header header{};
    header.magic            = magic;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = type;
    header.payload_size     = payload_size;
    std::array<uint8_t, WD_TCP_HEADER_WIRE_SIZE> bytes{};
    CHECK(wd_tcp_header_encode(bytes.data(), &header));
    return bytes;
}

wd_client_hello_payload valid_hello() {
    wd_client_hello_payload hello{};
    hello.client_udp_port      = 6000;
    hello.requested_capture_fps = 60;
    hello.video_mode           = WD_VIDEO_MODE_OFF;
    return hello;
}

void test_fragmented_hello() {
    SocketPair pair;
    wd_tcp_reader reader{};
    wd_tcp_reader_init(&reader, WD_TCP_MAX_PAYLOAD_SIZE);
    const auto header = encode_header(WD_MSG_CLIENT_HELLO, sizeof(wd_client_hello_payload));
    const auto hello  = valid_hello();
    wd_tcp_message message{};
    uint64_t now = 1000;

    for (uint8_t byte : header)
    {
        CHECK(write(pair.fd[0], &byte, 1) == 1);
        const auto status = wd_tcp_reader_receive(&reader, pair.fd[1], now, 100, 10000, &message);
        CHECK(status == WD_TCP_READER_NEED_MORE);
        now += 10;
    }
    const auto* payload = reinterpret_cast<const uint8_t*>(&hello);
    for (size_t i = 0; i < sizeof(hello); ++i)
    {
        CHECK(write(pair.fd[0], payload + i, 1) == 1);
        const auto status = wd_tcp_reader_receive(&reader, pair.fd[1], now, 100, 10000, &message);
        if (i + 1 == sizeof(hello))
        {
            CHECK(status == WD_TCP_READER_MESSAGE);
        }
        else
        {
            CHECK(status == WD_TCP_READER_NEED_MORE);
        }
        now += 10;
    }
    CHECK(message.message_type == WD_MSG_CLIENT_HELLO);
    CHECK(message.payload_size == sizeof(hello));
    CHECK(std::memcmp(message.payload, &hello, sizeof(hello)) == 0);
    wd_tcp_message_release(&message);
    wd_tcp_reader_destroy(&reader);
}

void test_frame_lifetime_and_idle_timeout() {
    SocketPair pair;
    wd_tcp_reader reader{};
    wd_tcp_reader_init(&reader, WD_TCP_MAX_PAYLOAD_SIZE);
    const auto header = encode_header(WD_MSG_CLIENT_HELLO, sizeof(wd_client_hello_payload));
    CHECK(write(pair.fd[0], header.data(), 1) == 1);
    wd_tcp_message message{};
    CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 100, 50, 1000, &message) == WD_TCP_READER_NEED_MORE);
    CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 151, 50, 1000, &message) == WD_TCP_READER_TIMED_OUT);
    wd_tcp_reader_destroy(&reader);

    wd_tcp_reader_init(&reader, WD_TCP_MAX_PAYLOAD_SIZE);
    CHECK(write(pair.fd[0], header.data(), 1) == 1);
    CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 1000, 100, 300, &message) == WD_TCP_READER_NEED_MORE);
    for (size_t i = 1; i < 7; ++i)
    {
        CHECK(write(pair.fd[0], header.data() + i, 1) == 1);
        CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 1000 + i * 40, 100, 300, &message) == WD_TCP_READER_NEED_MORE);
    }
    /* Progress extends the idle deadline, but never the absolute frame lifetime. */
    CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 1300, 100, 300, &message) == WD_TCP_READER_TIMED_OUT);
    wd_tcp_reader_destroy(&reader);
}

void test_invalid_and_partial_frames() {
    {
        SocketPair pair;
        wd_tcp_reader reader{};
        wd_tcp_reader_init(&reader, WD_TCP_MAX_PAYLOAD_SIZE);
        const auto header = encode_header(WD_MSG_CLIENT_HELLO, sizeof(wd_client_hello_payload), 0xdeadbeefu);
        CHECK(write(pair.fd[0], header.data(), header.size()) == static_cast<ssize_t>(header.size()));
        wd_tcp_message message{};
        CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 1, 100, 100, &message) == WD_TCP_READER_INVALID_FRAME);
        wd_tcp_reader_destroy(&reader);
    }
    {
        SocketPair pair;
        wd_tcp_reader reader{};
        wd_tcp_reader_init(&reader, WD_TCP_MAX_PAYLOAD_SIZE);
        const auto header = encode_header(WD_MSG_CLIENT_HELLO, sizeof(wd_client_hello_payload));
        CHECK(write(pair.fd[0], header.data(), header.size()) == static_cast<ssize_t>(header.size()));
        const uint8_t partial[3]{};
        CHECK(write(pair.fd[0], partial, sizeof(partial)) == static_cast<ssize_t>(sizeof(partial)));
        close(pair.fd[0]);
        pair.fd[0] = -1;
        wd_tcp_message message{};
        CHECK(wd_tcp_reader_receive(&reader, pair.fd[1], 1, 100, 100, &message) == WD_TCP_READER_PEER_CLOSED);
        wd_tcp_reader_destroy(&reader);
    }
}

wd_aux_channel_policy policy() {
    wd_aux_channel_policy value{};
    value.session_id       = 7;
    value.connection_token = UINT64_C(0x1122334455667788);
    value.video_negotiated = true;
    value.video_codecs     = WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265;
    value.video_transport  = WD_VIDEO_TRANSPORT_TCP;
    value.audio_negotiated = true;
    value.audio_codec      = WD_AUDIO_CODEC_OPUS;
    value.audio_transport  = WD_AUDIO_TRANSPORT_TCP;
    return value;
}

void test_auxiliary_channel_binding() {
    auto p = policy();
    wd_input_channel_hello_payload input{p.session_id, p.connection_token};
    CHECK(wd_aux_channel_validate_hello(WD_MSG_INPUT_CHANNEL_HELLO, &input, sizeof(input), &p) == WD_AUX_CHANNEL_INPUT);
    p.input_bound = true;
    CHECK(wd_aux_channel_validate_hello(WD_MSG_INPUT_CHANNEL_HELLO, &input, sizeof(input), &p) == WD_AUX_CHANNEL_INVALID);
    p.input_bound = false;
    input.connection_token++;
    CHECK(wd_aux_channel_validate_hello(WD_MSG_INPUT_CHANNEL_HELLO, &input, sizeof(input), &p) == WD_AUX_CHANNEL_INVALID);

    wd_selection_channel_hello_payload selection{p.session_id, p.connection_token};
    CHECK(wd_aux_channel_validate_hello(WD_MSG_SELECTION_CHANNEL_HELLO, &selection, sizeof(selection), &p) ==
          WD_AUX_CHANNEL_SELECTION);

    wd_video_channel_hello_payload video{p.session_id, p.connection_token, WD_VIDEO_CODEC_H265, WD_VIDEO_TRANSPORT_TCP};
    CHECK(wd_aux_channel_validate_hello(WD_MSG_VIDEO_CHANNEL_HELLO, &video, sizeof(video), &p) == WD_AUX_CHANNEL_VIDEO);
    video.video_codecs = 1u << 31u;
    CHECK(wd_aux_channel_validate_hello(WD_MSG_VIDEO_CHANNEL_HELLO, &video, sizeof(video), &p) == WD_AUX_CHANNEL_INVALID);
    video.video_codecs = WD_VIDEO_CODEC_H264;
    p.video_negotiated = false;
    CHECK(wd_aux_channel_validate_hello(WD_MSG_VIDEO_CHANNEL_HELLO, &video, sizeof(video), &p) == WD_AUX_CHANNEL_INVALID);

    p = policy();
    wd_audio_channel_hello_payload audio{p.session_id, p.connection_token, WD_AUDIO_CODEC_OPUS, WD_AUDIO_TRANSPORT_TCP};
    CHECK(wd_aux_channel_validate_hello(WD_MSG_AUDIO_CHANNEL_HELLO, &audio, sizeof(audio), &p) == WD_AUX_CHANNEL_AUDIO);
    audio.audio_transport++;
    CHECK(wd_aux_channel_validate_hello(WD_MSG_AUDIO_CHANNEL_HELLO, &audio, sizeof(audio), &p) == WD_AUX_CHANNEL_INVALID);

    CHECK(wd_aux_channel_validate_hello(WD_MSG_PRIMARY_SET, &selection, sizeof(selection), &p) == WD_AUX_CHANNEL_INVALID);
}

void test_channel_dispatch_matrix() {
    const wd_protocol_channel channels[] = {WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_CHANNEL_SELECTION,
                                             WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_CHANNEL_AUDIO};
    for (const auto channel : channels)
    {
        const bool primary_allowed = wd_protocol_message_allowed(WD_MSG_PRIMARY_SET, channel, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                  WD_PROTOCOL_SERVER_TO_CLIENT,
                                                                  sizeof(wd_selection_payload_header));
        CHECK(primary_allowed == (channel == WD_PROTOCOL_CHANNEL_SELECTION));
        const bool video_allowed = wd_protocol_message_allowed(WD_MSG_VIDEO_FRAME, channel, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                WD_PROTOCOL_SERVER_TO_CLIENT,
                                                                sizeof(wd_video_frame_payload_header));
        CHECK(video_allowed == (channel == WD_PROTOCOL_CHANNEL_VIDEO));
    }
}

} // namespace

int main() {
    test_fragmented_hello();
    test_frame_lifetime_and_idle_timeout();
    test_invalid_and_partial_frames();
    test_auxiliary_channel_binding();
    test_channel_dispatch_matrix();
    return 0;
}
