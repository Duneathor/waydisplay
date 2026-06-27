#include "waydisplay/wd_config.h"
#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_dispatch.h"
#include "waydisplay/wd_tile.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_protocol_version_and_header_sizes() {
    require(WD_PROTOCOL_VERSION == 0, "undeployed protocol should reset to version zero");
    require(WD_UDP_TILE_HEADER_MIN_SIZE == 36, "canonical tile header size");
    require(WD_UDP_TILE_HEADER_MAX_SIZE == 44, "correlated tile header size");
}


template <typename T>
void require_fixed_wire_layout(uint16_t message_type, const T& source, const char* message) {
    uint32_t wire_size = 0;
    require(wd_protocol_payload_wire_size(message_type, &source, sizeof(source), &wire_size), message);
    require(wire_size == sizeof(source), "fixed wire size should match the packed protocol-zero structure");
    require(wd_protocol_payload_validate(message_type, &source, sizeof(source)), message);
}

void test_protocol_zero_native_wire_layout() {
    wd_tcp_header header{};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = WD_MSG_SERVER_CONFIG;
    header.payload_size     = 0x12345678u;
    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE]{};
    require(wd_tcp_header_encode(wire_header, &header), "TCP header should encode");
    const uint8_t expected_header[WD_TCP_HEADER_WIRE_SIZE] = {0x57, 0x44, 0x43, 0x54, 0x00, 0x00,
                                                              0x02, 0x00, 0x78, 0x56, 0x34, 0x12};
    require(std::memcmp(wire_header, expected_header, sizeof(expected_header)) == 0,
            "TCP header should use the documented little-endian layout");
    wd_tcp_header decoded_header{};
    require(wd_tcp_header_decode(wire_header, &decoded_header), "TCP header should decode");
    require(decoded_header.magic == header.magic && decoded_header.protocol_version == 0 &&
                decoded_header.message_type == header.message_type && decoded_header.payload_size == header.payload_size,
            "TCP header fields should round trip");

    wd_input_channel_hello_payload input_hello{};
    input_hello.session_id       = 0x7au;
    input_hello.connection_token = 0x0102030405060708ull;
    const uint8_t expected_input_hello[sizeof(input_hello)] = {0x7a, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    require(std::memcmp(&input_hello, expected_input_hello, sizeof(input_hello)) == 0,
            "packed input-channel hello should match the protocol-zero golden bytes");

    wd_config_applied_payload applied{};
    applied.session_id       = 0x11u;
    applied.connection_token = 0x1122334455667788ull;
    applied.config_epoch     = 0x0102030405060708ull;
    const uint8_t expected_applied[sizeof(applied)] = {0x11, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                                                       0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    require(std::memcmp(&applied, expected_applied, sizeof(applied)) == 0,
            "packed config-applied payload should match the protocol-zero golden bytes");

    require_fixed_wire_layout(WD_MSG_CLIENT_HELLO, wd_client_hello_payload{}, "client hello codec");
    require_fixed_wire_layout(WD_MSG_SERVER_CONFIG, wd_server_config_payload{}, "server config codec");
    require_fixed_wire_layout(WD_MSG_KEYBOARD_KEY, wd_keyboard_event_payload{}, "keyboard codec");
    require_fixed_wire_layout(WD_MSG_POINTER_EVENT, wd_pointer_event_payload{}, "pointer codec");
    require_fixed_wire_layout(WD_MSG_MTU_PROBE_START, wd_mtu_probe_start_payload{}, "MTU start codec");
    require_fixed_wire_layout(WD_MSG_MTU_PROBE_RESULT, wd_mtu_probe_result_payload{}, "MTU result codec");
    require_fixed_wire_layout(WD_MSG_CURSOR_SHAPE, wd_cursor_shape_payload{}, "cursor codec");
    require_fixed_wire_layout(WD_MSG_DISPLAY_RESIZE, wd_display_resize_payload{}, "resize codec");
    require_fixed_wire_layout(WD_MSG_THROUGHPUT_PROBE_START, wd_throughput_probe_start_payload{}, "throughput start codec");
    require_fixed_wire_layout(WD_MSG_THROUGHPUT_PROBE_RESULT, wd_throughput_probe_result_payload{}, "throughput result codec");
    require_fixed_wire_layout(WD_MSG_INPUT_CHANNEL_HELLO, wd_input_channel_hello_payload{}, "input hello codec");
    require_fixed_wire_layout(WD_MSG_SELECTION_CHANNEL_HELLO, wd_selection_channel_hello_payload{}, "selection hello codec");
    require_fixed_wire_layout(WD_MSG_CLIENT_STATS, wd_client_stats_payload{}, "client stats codec");
    require_fixed_wire_layout(WD_MSG_LINK_PROBE_PING, wd_link_probe_payload{}, "link ping codec");
    require_fixed_wire_layout(WD_MSG_LINK_PROBE_PONG, wd_link_probe_payload{}, "link pong codec");
    require_fixed_wire_layout(WD_MSG_VIDEO_CHANNEL_HELLO, wd_video_channel_hello_payload{}, "video hello codec");
    require_fixed_wire_layout(WD_MSG_CONFIG_APPLIED, wd_config_applied_payload{}, "config applied codec");
    require_fixed_wire_layout(WD_MSG_AUDIO_CHANNEL_HELLO, wd_audio_channel_hello_payload{}, "audio hello codec");
    require_fixed_wire_layout(WD_MSG_AUDIO_CONFIG, wd_audio_config_payload{}, "audio config codec");

    std::array<uint8_t, sizeof(wd_tile_summary_payload_header) + 2 * sizeof(wd_tile_generation_entry)> summary{};
    auto* summary_header       = reinterpret_cast<wd_tile_summary_payload_header*>(summary.data());
    summary_header->tile_count = 2;
    auto* summary_entries = reinterpret_cast<wd_tile_generation_entry*>(summary.data() + sizeof(*summary_header));
    summary_entries[0]    = {1, 0x0102030405060708ull};
    summary_entries[1]    = {2, 9};
    uint32_t wire_size = 0;
    require(wd_protocol_payload_wire_size(WD_MSG_TILE_GENERATION_SUMMARY, summary.data(), summary.size(), &wire_size),
            "summary wire layout should accept repeated entries");
    require(wire_size == summary.size() &&
                wd_protocol_payload_validate(WD_MSG_TILE_GENERATION_SUMMARY, summary.data(), summary.size()),
            "summary wire layout should preserve repeated entries in place");
    require(!wd_protocol_payload_size_is_valid(WD_MSG_TILE_GENERATION_SUMMARY,
                                                    sizeof(wd_tile_summary_payload_header) + 1u),
            "summary dispatch metadata should reject a partial repeated entry");

    std::array<uint8_t, sizeof(wd_selection_payload_header) + 4> selection{};
    auto* selection_header     = reinterpret_cast<wd_selection_payload_header*>(selection.data());
    selection_header->mime_type = WD_SELECTION_MIME_TEXT_UTF8;
    selection_header->data_size = 4;
    std::memcpy(selection.data() + sizeof(*selection_header), "test", 4);
    require(wd_protocol_payload_wire_size(WD_MSG_CLIPBOARD_SET, selection.data(), selection.size(), &wire_size),
            "selection wire layout should accept opaque data");
    require(wire_size == selection.size() && wd_protocol_payload_validate(WD_MSG_CLIPBOARD_SET, selection.data(), selection.size()),
            "selection wire layout should preserve opaque bytes in place");

    require(wd_protocol_payload_wire_size(WD_MSG_CLIPBOARD_REQUEST, nullptr, 0, &wire_size) && wire_size == 0,
            "empty request codec should accept a zero-size payload");
    require(!wd_protocol_payload_wire_size(0x7fffu, nullptr, 0, &wire_size), "unknown message types should not have an implicit codec");
}

void test_typed_protocol_dispatch() {
    const uint16_t message_types[] = {
        WD_MSG_CLIENT_HELLO,           WD_MSG_SERVER_CONFIG,          WD_MSG_TILE_GENERATION_SUMMARY,
        WD_MSG_TILE_REPAIR_REQUEST,    WD_MSG_KEYBOARD_KEY,           WD_MSG_POINTER_EVENT,
        WD_MSG_MTU_PROBE_START,        WD_MSG_MTU_PROBE_RESULT,       WD_MSG_CLIPBOARD_SET,
        WD_MSG_CLIPBOARD_REQUEST,      WD_MSG_PRIMARY_SET,            WD_MSG_PRIMARY_REQUEST,
        WD_MSG_CURSOR_SHAPE,           WD_MSG_DISPLAY_RESIZE,         WD_MSG_THROUGHPUT_PROBE_START,
        WD_MSG_THROUGHPUT_PROBE_RESULT, WD_MSG_INPUT_CHANNEL_HELLO,    WD_MSG_SELECTION_CHANNEL_HELLO,
        WD_MSG_CLIENT_STATS,           WD_MSG_LINK_PROBE_PING,        WD_MSG_LINK_PROBE_PONG,
        WD_MSG_VIDEO_CHANNEL_HELLO,    WD_MSG_VIDEO_FRAME,            WD_MSG_CONFIG_APPLIED,
        WD_MSG_AUDIO_CHANNEL_HELLO,    WD_MSG_AUDIO_CONFIG,           WD_MSG_AUDIO_PACKET,
        WD_MSG_VIDEO_FEEDBACK,
    };
    for (const uint16_t message_type : message_types)
    {
        require(wd_protocol_message_descriptor_find(message_type) != nullptr, "every declared message should have a dispatch descriptor");
        require(std::strcmp(wd_protocol_message_name(message_type), "WD_MSG_UNKNOWN") != 0,
                "every declared message should have a dispatch name");
    }

    require(wd_protocol_message_allowed(WD_MSG_CLIENT_HELLO, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION,
                                        WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_client_hello_payload)),
            "client hello should be legal only during control negotiation");
    require(!wd_protocol_message_allowed(WD_MSG_CLIENT_HELLO, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_client_hello_payload)),
            "client hello should be rejected after negotiation");
    require(wd_protocol_message_allowed(WD_MSG_KEYBOARD_KEY, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_PHASE_ESTABLISHED,
                                        WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_keyboard_event_payload)),
            "keyboard input should be legal on the input channel");
    require(!wd_protocol_message_allowed(WD_MSG_KEYBOARD_KEY, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_keyboard_event_payload)),
            "keyboard input should be rejected on the control channel");
    require(wd_protocol_message_allowed(WD_MSG_CLIPBOARD_SET, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
                                        WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_selection_payload_header)),
            "clipboard updates should be legal on the selection channel");
    require(!wd_protocol_message_allowed(WD_MSG_CLIPBOARD_SET, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_selection_payload_header)),
            "clipboard updates should be rejected on the control channel");
    require(!wd_protocol_message_allowed(WD_MSG_KEYBOARD_KEY, WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_keyboard_event_payload)),
            "keyboard input should be rejected on the video channel");
    require(wd_protocol_message_allowed(WD_MSG_VIDEO_FRAME, WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                        WD_PROTOCOL_SERVER_TO_CLIENT, sizeof(wd_video_frame_payload_header)),
            "video frame should be legal from server to client");
    require(!wd_protocol_message_allowed(WD_MSG_VIDEO_FRAME, WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_video_frame_payload_header)),
            "video frame should be rejected in the reverse direction");
    require(wd_protocol_message_allowed(WD_MSG_VIDEO_FEEDBACK, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
                                        WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_video_feedback_payload)),
            "video feedback should be legal on established client control");
    require(!wd_protocol_message_allowed(WD_MSG_VIDEO_FEEDBACK, WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_CLIENT_TO_SERVER, sizeof(wd_video_feedback_payload)),
            "video feedback should be rejected on the video channel");
    require(!wd_protocol_message_allowed(WD_MSG_AUDIO_PACKET, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                         WD_PROTOCOL_SERVER_TO_CLIENT,
                                         sizeof(wd_audio_packet_payload_header) + WD_AUDIO_PACKET_MAX_PAYLOAD_BYTES + 1u),
            "audio payloads above the channel cap should be rejected");
    require(wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                            WD_PROTOCOL_SERVER_TO_CLIENT) ==
                sizeof(wd_video_frame_payload_header) + WD_VIDEO_FRAME_MAX_PAYLOAD_BYTES,
            "video channel limit should come from the dispatch table");
    require(wd_protocol_message_descriptor_find(0x7fffu) == nullptr, "unknown message should not have a dispatch descriptor");
}

void test_media_clock_helpers() {
    require(wd_media_ns_to_usec(2000000000ull, 1000000000ull) == 1000000ull,
            "media microseconds should be relative to the connection origin");
    require(wd_media_ns_to_samples(1020000000ull, 1000000000ull, 48000) == 960, "20 ms should map to 960 Opus samples");
    require(wd_media_usec_to_samples(20000, 48000) == 960, "video PTS should map to the Opus sample clock exactly");
    require(wd_media_local_deadline_ns(5000, 20) == 25000, "remote media time should map onto a client-local origin");
}

void test_tile_size_round_trip() {
    struct Case {
        uint16_t width;
        uint16_t height;
        uint8_t  code;
    };
    const Case cases[] = {{128, 64, WD_TILE_128x64}, {64, 64, WD_TILE_64x64}, {32, 32, WD_TILE_32x32}, {16, 16, WD_TILE_16x16}};
    for (const Case& item : cases)
    {
        uint8_t code = 0xff;
        require(wd_tile_size_code_for_dimensions(item.width, item.height, &code), "dimension-to-code mapping");
        require(code == item.code, "dimension-to-code value");
        uint16_t width  = 0;
        uint16_t height = 0;
        require(wd_tile_dimensions_for_size_code(item.code, &width, &height), "code-to-dimension mapping");
        require(width == item.width && height == item.height, "tile dimensions should round trip");
    }
    uint8_t code = 0xff;
    require(!wd_tile_size_code_for_dimensions(48, 48, &code), "unsupported dimensions should be rejected");
}

std::vector<uint8_t> encode_packet(wd_udp_tile_packet_decoded header) {
    std::vector<uint8_t> packet(wd_udp_tile_header_size_for_flags(header.flags) + header.payload_size, 0xa5);
    require(wd_udp_tile_packet_encode_header(packet.data(), packet.size(), &header), "tile header should encode");
    return packet;
}

void test_base_header_round_trip() {
    wd_udp_tile_packet_decoded source{};
    source.session_id        = 7;
    source.connection_token  = 0x1122334455667788ull;
    source.content_epoch     = 5;
    source.flags             = WD_UDP_TILE_FLAG_COMPRESSED;
    source.tile_size         = WD_TILE_32x32;
    source.tile_pkt_id       = 1;
    source.tile_id           = 9;
    source.tile_pkt_count    = 3;
    source.payload_size      = 13;
    source.tile_payload_size = 29;
    source.tile_generation   = 42;

    const auto                 packet = encode_packet(source);
    wd_udp_tile_packet_decoded decoded{};
    require(wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "base header should decode");
    require(decoded.session_id == source.session_id && decoded.connection_token == source.connection_token &&
                decoded.content_epoch == source.content_epoch && decoded.flags == source.flags,
            "base identity fields");
    require(decoded.tile_size == source.tile_size && decoded.tile_id == source.tile_id, "base tile fields");
    require(decoded.tile_pkt_count == 3 && decoded.tile_pkt_id == 1, "base fragment fields");
    require(decoded.payload_size == 13 && decoded.tile_payload_size == 29, "base payload fields");
    require(decoded.tile_generation == 42 && decoded.input_sequence == 0, "base generation fields");
    require(decoded.header_size == WD_UDP_TILE_HEADER_MIN_SIZE, "base header decoded size");
    require(wd_udp_tile_packet_is_compressed(&decoded), "compressed flag should decode");
}

void test_input_extension_round_trip() {
    wd_udp_tile_packet_decoded source{};
    source.session_id        = 3;
    source.connection_token  = 0x8877665544332211ull;
    source.content_epoch     = 5;
    source.flags             = WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
    source.tile_size         = WD_TILE_16x16;
    source.tile_pkt_id       = 0;
    source.tile_id           = 4;
    source.tile_pkt_count    = 2;
    source.payload_size      = 400;
    source.tile_payload_size = 700;
    source.tile_generation   = 12;
    source.input_sequence    = 777;

    const auto                 packet = encode_packet(source);
    wd_udp_tile_packet_decoded decoded{};
    require(wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "extended header should decode");
    require(decoded.header_size == WD_UDP_TILE_HEADER_MAX_SIZE, "extension should enlarge only packet zero");
    require(decoded.input_sequence == 777, "input sequence should round trip");
    require(!wd_udp_tile_packet_is_compressed(&decoded), "uncompressed flag should decode");
}

void test_rejects_invalid_extensions_and_flags() {
    wd_udp_tile_packet_decoded header{};
    header.session_id        = 1;
    header.connection_token  = 9;
    header.content_epoch     = 5;
    header.flags             = WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
    header.tile_size         = WD_TILE_16x16;
    header.tile_pkt_id       = 1;
    header.tile_id           = 0;
    header.tile_pkt_count    = 2;
    header.payload_size      = 10;
    header.tile_payload_size = 20;
    header.tile_generation   = 1;
    header.input_sequence    = 9;
    std::array<uint8_t, WD_UDP_TILE_HEADER_MAX_SIZE> encoded{};
    require(!wd_udp_tile_packet_encode_header(encoded.data(), encoded.size(), &header),
            "input extension should be legal only on packet zero");

    header.tile_pkt_id = 0;
    header.flags       = 0x80;
    require(!wd_udp_tile_packet_encode_header(encoded.data(), encoded.size(), &header), "unknown flags should be rejected");

    wd_udp_tile_packet_header wire{};
    wire.session_id        = 1;
    wire.connection_token  = 9;
    wire.content_epoch     = 5;
    wire.flags             = 0x80;
    wire.tile_pkt_count    = 1;
    wire.payload_size      = 1;
    wire.tile_payload_size = 1;
    wire.tile_generation   = 1;
    std::array<uint8_t, sizeof(wire) + 1> packet{};
    std::memcpy(packet.data(), &wire, sizeof(wire));
    wd_udp_tile_packet_decoded decoded{};
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "unknown wire flags should be rejected");

    wire.flags            = 0;
    wire.connection_token = 0;
    wire.content_epoch    = 5;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "zero connection token should be rejected");

    wire.connection_token = 9;
    wire.content_epoch    = 0;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "zero content epoch should be rejected");

    wire.content_epoch = 5;
    wire.session_id    = 0;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "zero tile session should be rejected");

    wire.session_id = 1;
    wire.tile_size  = 0xff;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "unknown tile size should be rejected");

    wire.tile_size         = WD_TILE_16x16;
    wire.payload_size      = 2;
    wire.tile_payload_size = 1;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "a fragment cannot exceed its declared tile payload");
}

void test_decode_rejects_trailing_bytes_and_reserved_data() {
    wd_udp_tile_packet_decoded header{};
    header.session_id        = 1;
    header.connection_token  = 9;
    header.content_epoch     = 5;
    header.tile_size         = WD_TILE_16x16;
    header.tile_pkt_count    = 1;
    header.payload_size      = 4;
    header.tile_payload_size = 4;
    header.tile_generation   = 1;
    auto packet              = encode_packet(header);
    packet.push_back(0);
    wd_udp_tile_packet_decoded decoded{};
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "trailing bytes should be rejected");

    packet.pop_back();
    auto* wire     = reinterpret_cast<wd_udp_tile_packet_header*>(packet.data());
    wire->reserved = 1;
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "reserved byte should be zero");
}

void test_fragment_layout_is_canonical() {
    wd_udp_tile_packet_decoded decoded{};
    decoded.header_size       = WD_UDP_TILE_HEADER_MIN_SIZE;
    decoded.tile_payload_size = 1024;
    decoded.tile_pkt_count    = 3;
    decoded.tile_pkt_id       = 0;
    decoded.payload_size      = 400;
    require(wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "first full fragment should be valid");
    decoded.tile_pkt_id  = 2;
    decoded.payload_size = 224;
    require(wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "final remainder fragment should be valid");
    decoded.payload_size = 223;
    require(!wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "short final fragment should be rejected");
    decoded.payload_size   = 224;
    decoded.tile_pkt_count = 4;
    require(!wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "noncanonical packet count should be rejected");
    decoded.tile_pkt_count    = 3;
    decoded.tile_payload_size = 1023;
    require(!wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "declared total payload must match validated payload");
}

void test_protocol_zero_strict_payload_helpers() {
    require(sizeof(wd_server_config_payload) == 101, "protocol-zero server config should include media and audio negotiation");
    require(sizeof(wd_config_applied_payload) == 17, "protocol-zero config ACK should include config epoch");
    require(wd_fixed_payload_size_is_valid(17, sizeof(wd_config_applied_payload)), "fixed payload helper should accept exact size");
    require(!wd_fixed_payload_size_is_valid(18, sizeof(wd_config_applied_payload)), "fixed payload helper should reject trailing bytes");
    wd_config_applied_payload applied{4, 5, 6};
    require(wd_config_applied_matches(&applied, 4, 5, 6), "config ACK should match the exact connection and config epoch");
    require(!wd_config_applied_matches(&applied, 4, 5, 7), "stale config ACK epoch should be rejected");
    require(wd_counted_payload_size_is_valid(28 + 20, 28, 2, 10), "counted payload helper should accept exact element count");
    require(!wd_counted_payload_size_is_valid(28 + 21, 28, 2, 10), "counted payload helper should reject trailing bytes");
    require(wd_tile_summary_count_is_valid(0, 64, 64), "full summaries should cover every configured tile");
    require(!wd_tile_summary_count_is_valid(0, 63, 64), "short full summaries should be rejected");
    require(wd_tile_summary_count_is_valid(WD_TILE_SUMMARY_DELTA, 0, 64), "empty delta summaries should remain canonical");
    require(!wd_tile_summary_count_is_valid(WD_TILE_SUMMARY_DELTA, 65, 64), "summary counts cannot exceed the configured tile count");
    require(wd_tile_repair_count_is_valid(1, 64), "nonempty bounded repair requests should validate");
    require(!wd_tile_repair_count_is_valid(0, 64), "empty repair requests should be rejected");
    require(!wd_tile_repair_count_is_valid(65, 64), "repair requests cannot exceed the configured tile count");
}

void test_client_hello_strict_validation() {
    wd_client_hello_payload hello{};
    hello.client_udp_port = 6000;
    hello.video_mode      = WD_VIDEO_MODE_AUTO;
    require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "non-video client hello should validate");

    hello.capabilities    = WD_CLIENT_CAP_VIDEO_STREAM;
    hello.video_codecs    = WD_VIDEO_CODEC_H265;
    hello.video_transport = WD_VIDEO_TRANSPORT_TCP;
    require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "canonical video client hello should validate");

    hello.capabilities |= WD_CLIENT_CAP_AUDIO_STREAM;
    hello.audio_codecs            = WD_AUDIO_CODEC_OPUS;
    hello.audio_transport         = WD_AUDIO_TRANSPORT_TCP;
    hello.audio_max_channels      = 2;
    hello.audio_target_latency_ms = 60;
    require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "canonical audio and video client hello should validate");

    hello.audio_max_channels = 0;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "audio capability requires a channel count");
    hello.audio_max_channels      = 2;
    hello.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_MIN;
    require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "minimum audio latency should validate");
    hello.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_MIN - 1u;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "audio latency below the protocol minimum should be rejected");
    hello.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;

    hello.video_reserved = 1;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "reserved client hello bits should be rejected");
    hello.video_reserved = 0;
    hello.desired_width  = WD_MAX_RENDER_WIDTH + 1u;
    hello.desired_height = 1080;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)),
            "client hello should reject render geometry above the protocol limit");
    hello.desired_width  = 0;
    hello.desired_height = 0;
    hello.capabilities |= 0x80000000u;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)), "unknown client capabilities should be rejected");
    hello.capabilities = WD_CLIENT_CAP_VIDEO_STREAM | WD_CLIENT_CAP_AUDIO_STREAM;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello) + 1), "client hello trailing bytes should be rejected");
}

void test_video_frame_strict_validation() {
    wd_video_frame_payload_header frame{};
    frame.session_id       = 1;
    frame.connection_token = 2;
    frame.content_epoch    = 3;
    frame.codec            = WD_VIDEO_CODEC_H265;
    frame.flags            = WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_CONFIG;
    frame.frame_id         = 1;
    frame.pts_usec         = 0;
    frame.width            = 1280;
    frame.height           = 720;
    frame.coded_width      = 1280;
    frame.coded_height     = 720;
    frame.data_size        = 4;
    require(wd_video_frame_payload_size_is_valid(&frame, sizeof(frame) + 4), "canonical video data frame should validate");

    frame.flags |= 0x8000u;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame) + 4), "unknown video flags should be rejected");
    frame.flags = WD_VIDEO_FRAME_CONFIG;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame) + 4), "config video frames must also be keyframes");

    frame.flags     = WD_VIDEO_FRAME_END_OF_STREAM;
    frame.frame_id  = 0;
    frame.data_size = 0;
    require(wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)), "canonical video EOS should validate");
    frame.flags |= WD_VIDEO_FRAME_KEYFRAME;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)), "control frames cannot also be keyframes");

    frame.flags = WD_VIDEO_FRAME_RESIZE;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)), "resize controls must also terminate the old video stream");
    frame.flags = WD_VIDEO_FRAME_END_OF_STREAM | WD_VIDEO_FRAME_RESIZE;
    require(wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)), "canonical resize EOS should validate");
    frame.frame_id = 7;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)), "control frames must not carry a data sequence id");
}

void test_audio_payload_strict_validation() {
    wd_audio_config_payload config{};
    config.session_id          = 1;
    config.connection_token    = 2;
    config.audio_epoch         = 3;
    config.media_clock_id      = 4;
    config.codec               = WD_AUDIO_CODEC_OPUS;
    config.sample_rate         = WD_AUDIO_SAMPLE_RATE_DEFAULT;
    config.channels            = 2;
    config.frame_samples       = WD_AUDIO_FRAME_SAMPLES_DEFAULT;
    config.codec_delay_samples = 312;
    config.target_bitrate      = WD_AUDIO_BITRATE_DEFAULT;
    require(wd_audio_config_payload_is_valid(&config, sizeof(config)), "canonical audio config should validate");
    config.channels = 3;
    require(!wd_audio_config_payload_is_valid(&config, sizeof(config)), "audio config should reject unsupported channel counts");
    config.channels    = 2;
    config.sample_rate = 44100;
    require(!wd_audio_config_payload_is_valid(&config, sizeof(config)), "audio config should reject a non-48k Opus clock");

    wd_audio_packet_payload_header packet{};
    packet.session_id       = 1;
    packet.connection_token = 2;
    packet.audio_epoch      = 3;
    packet.media_clock_id   = 4;
    packet.sequence         = 1;
    packet.pts_samples      = 0;
    packet.duration_samples = WD_AUDIO_FRAME_SAMPLES_DEFAULT;
    packet.flags            = WD_AUDIO_PACKET_DISCONTINUITY;
    packet.data_size        = 24;
    require(wd_audio_packet_payload_size_is_valid(&packet, sizeof(packet) + packet.data_size),
            "first Opus packet at media time zero should validate");
    packet.data_size = 0;
    require(!wd_audio_packet_payload_size_is_valid(&packet, sizeof(packet)), "normal audio packet must contain codec data");
    packet.flags            = WD_AUDIO_PACKET_END_OF_STREAM;
    packet.duration_samples = 0;
    require(wd_audio_packet_payload_size_is_valid(&packet, sizeof(packet)), "canonical audio EOS should validate");
    packet.flags |= 0x8000u;
    require(!wd_audio_packet_payload_size_is_valid(&packet, sizeof(packet)), "unknown audio flags should be rejected");
}

void test_input_payload_strict_validation() {
    wd_keyboard_event_payload key{};
    key.session_id          = 1;
    key.connection_token    = 2;
    key.client_timestamp_ns = 3;
    key.input_sequence      = 4;
    key.evdev_key_code      = 30;
    key.pressed             = 1;
    require(wd_keyboard_event_payload_is_valid(&key, sizeof(key)), "canonical keyboard payload should validate");
    key.pressed = 2;
    require(!wd_keyboard_event_payload_is_valid(&key, sizeof(key)), "invalid keyboard state should be rejected");

    wd_pointer_event_payload pointer{};
    pointer.session_id          = 1;
    pointer.connection_token    = 2;
    pointer.client_timestamp_ns = 3;
    pointer.input_sequence      = 4;
    pointer.event_type          = WD_POINTER_EVENT_AXIS;
    pointer.axis                = WD_POINTER_AXIS_VERTICAL;
    pointer.axis_value          = -120;
    require(wd_pointer_event_payload_is_valid(&pointer, sizeof(pointer)), "canonical pointer axis payload should validate");
    pointer.axis_value = 0;
    require(!wd_pointer_event_payload_is_valid(&pointer, sizeof(pointer)), "zero-value pointer axis payload should be rejected");
    pointer.axis_value = -120;
    pointer.modifiers  = 0x10;
    require(!wd_pointer_event_payload_is_valid(&pointer, sizeof(pointer)), "unknown pointer modifier bits should be rejected");
}

void test_channel_specific_tcp_payload_limits() {
    int sockets[2] = {-1, -1};
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create TCP framing socket pair");

    const uint32_t       large_size = WD_TCP_MAX_PAYLOAD_SIZE + 4096u;
    std::vector<uint8_t> payload(large_size, 0x5a);
    std::thread          sender([&]() {
        require(wd_send_tcp_message(sockets[0], WD_MSG_VIDEO_FRAME, payload.data(), large_size), "send channel-specific large frame");
        close(sockets[0]);
    });

    uint16_t type          = 0;
    uint8_t* received      = nullptr;
    uint32_t received_size = 0;
    require(wd_recv_tcp_message_limited(sockets[1], large_size, &type, &received, &received_size),
            "video channel limit should allow payloads above the control limit");
    require(type == WD_MSG_VIDEO_FRAME && received_size == large_size, "large channel payload should retain framing metadata");
    require(received && received[0] == 0x5a && received[large_size - 1] == 0x5a, "large channel payload should be received intact");
    std::free(received);
    close(sockets[1]);
    sender.join();

    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create limit rejection socket pair");
    wd_tcp_header header{};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = WD_MSG_VIDEO_FRAME;
    header.payload_size     = WD_TCP_MAX_PAYLOAD_SIZE + 1u;
    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE]{};
    require(wd_tcp_header_encode(wire_header, &header), "encode oversized control header");
    require(wd_send_all(sockets[0], wire_header, sizeof(wire_header)), "send oversized control header");
    received      = reinterpret_cast<uint8_t*>(1);
    received_size = 1;
    require(!wd_recv_tcp_message(sockets[1], &type, &received, &received_size),
            "default control receiver should retain the strict 2 MiB limit");
    require(received == nullptr && received_size == 0, "failed receive should clear outputs");
    close(sockets[0]);
    close(sockets[1]);
}


void test_incremental_tcp_reader() {
    int sockets[2] = {-1, -1};
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create incremental reader socket pair");

    wd_tcp_header header{};
    header.magic            = WD_TCP_MAGIC;
    header.protocol_version = WD_PROTOCOL_VERSION;
    header.message_type     = WD_MSG_MTU_PROBE_START;
    header.payload_size     = sizeof(wd_mtu_probe_start_payload);

    uint8_t wire_header[WD_TCP_HEADER_WIRE_SIZE]{};
    require(wd_tcp_header_encode(wire_header, &header), "encode incremental TCP header");

    wd_tcp_reader reader{};
    wd_tcp_reader_init(&reader, 64);
    wd_tcp_message message{};

    require(wd_send_all(sockets[0], wire_header, 5), "send partial TCP header");
    require(wd_tcp_reader_receive(&reader, sockets[1], 100, 50, 500, &message) == WD_TCP_READER_NEED_MORE,
            "partial header should not block or complete");
    require(wd_tcp_reader_has_partial_frame(&reader), "partial header should retain reader state");
    require(wd_tcp_reader_deadline_ns(&reader) == 150, "partial header should start the idle deadline");
    require(wd_tcp_reader_receive(&reader, sockets[1], 151, 50, 500, &message) == WD_TCP_READER_TIMED_OUT,
            "stalled partial header should time out");

    wd_tcp_reader_reset(&reader);
    const uint8_t payload[sizeof(wd_mtu_probe_start_payload)] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    require(wd_send_all(sockets[0], wire_header + 5, sizeof(wire_header) - 5), "finish prior test header bytes");
    uint8_t discard[sizeof(wire_header) - 5]{};
    require(wd_recv_all(sockets[1], discard, sizeof(discard)), "discard prior test header remainder");

    require(wd_send_all(sockets[0], wire_header, sizeof(wire_header)), "send complete TCP header");
    require(wd_send_all(sockets[0], payload, 3), "send partial TCP payload");
    require(wd_tcp_reader_receive(&reader, sockets[1], 200, 50, 500, &message) == WD_TCP_READER_NEED_MORE,
            "partial payload should retain frame state");
    require(wd_send_all(sockets[0], payload + 3, sizeof(payload) - 3), "finish TCP payload");
    require(wd_tcp_reader_receive(&reader, sockets[1], 220, 50, 500, &message) == WD_TCP_READER_MESSAGE,
            "incremental reader should complete after remaining payload arrives");
    require(message.message_type == WD_MSG_MTU_PROBE_START && message.payload_size == sizeof(payload),
            "incremental reader should preserve framing metadata");
    require(std::memcmp(message.payload, payload, sizeof(payload)) == 0, "incremental reader should preserve payload bytes");
    wd_tcp_message_release(&message);

    wd_tcp_reader_reset(&reader);
    require(wd_send_all(sockets[0], wire_header, 4), "send slow-progress header prefix");
    require(wd_tcp_reader_receive(&reader, sockets[1], 400, 50, 500, &message) == WD_TCP_READER_NEED_MORE,
            "slow frame should start idle and total deadlines");
    require(wd_send_all(sockets[0], wire_header + 4, 4), "advance slow-progress header");
    require(wd_tcp_reader_receive(&reader, sockets[1], 440, 50, 500, &message) == WD_TCP_READER_NEED_MORE,
            "useful progress should refresh the idle deadline");
    require(wd_tcp_reader_deadline_ns(&reader) == 490, "idle deadline should follow the most recent progress");
    require(wd_tcp_reader_receive(&reader, sockets[1], 491, 50, 500, &message) == WD_TCP_READER_TIMED_OUT,
            "a frame should still time out after progress stops");

    wd_tcp_reader_reset(&reader);
    require(wd_send_all(sockets[0], wire_header, 4), "send total-lifetime header prefix");
    require(wd_tcp_reader_receive(&reader, sockets[1], 500, 50, 100, &message) == WD_TCP_READER_NEED_MORE,
            "total-lifetime frame should start");
    require(wd_send_all(sockets[0], wire_header + 4, 4), "advance total-lifetime frame");
    require(wd_tcp_reader_receive(&reader, sockets[1], 540, 50, 100, &message) == WD_TCP_READER_NEED_MORE,
            "progress should keep the idle deadline alive");
    require(wd_send_all(sockets[0], wire_header + 8, 4), "finish total-lifetime header bytes");
    require(wd_tcp_reader_receive(&reader, sockets[1], 601, 50, 100, &message) == WD_TCP_READER_TIMED_OUT,
            "hard frame lifetime should win even when unread progress is available");
    uint8_t total_lifetime_remainder[4]{};
    require(wd_recv_all(sockets[1], total_lifetime_remainder, sizeof(total_lifetime_remainder)),
            "discard bytes left after total-lifetime timeout");

    wd_tcp_reader_reset(&reader);
    header.payload_size = 65;
    require(wd_tcp_header_encode(wire_header, &header), "encode oversized incremental header");
    require(wd_send_all(sockets[0], wire_header, sizeof(wire_header)), "send oversized incremental header");
    require(wd_tcp_reader_receive(&reader, sockets[1], 300, 50, 500, &message) == WD_TCP_READER_INVALID_FRAME,
            "incremental reader should reject oversized payload before allocation");
    require(reader.payload == nullptr, "oversized incremental frame should not allocate payload memory");

    wd_tcp_reader_destroy(&reader);
    close(sockets[0]);
    close(sockets[1]);
}

void test_tile_count_helpers_reject_overflow() {
    require(wd_total_tiles_for_size_with_tile(WD_MAX_RENDER_WIDTH, WD_MAX_RENDER_HEIGHT, 16, 16) != 0,
            "maximum supported render geometry should fit the 16-bit tile grid");
    require(wd_total_tiles_for_size_with_tile(7680, 4320, 16, 16) == 0, "8K base-tile count must be rejected instead of truncating");
    require(wd_tiles_for_width_with_tile(UINT32_MAX, 1) == 0, "per-axis tile count overflow must be rejected");
}

} // namespace

int main() {
    test_protocol_version_and_header_sizes();
    test_protocol_zero_native_wire_layout();
    test_typed_protocol_dispatch();
    test_media_clock_helpers();
    test_tile_size_round_trip();
    test_base_header_round_trip();
    test_input_extension_round_trip();
    test_rejects_invalid_extensions_and_flags();
    test_decode_rejects_trailing_bytes_and_reserved_data();
    test_fragment_layout_is_canonical();
    test_protocol_zero_strict_payload_helpers();
    test_client_hello_strict_validation();
    test_video_frame_strict_validation();
    test_audio_payload_strict_validation();
    test_input_payload_strict_validation();
    test_channel_specific_tcp_payload_limits();
    test_incremental_tcp_reader();
    test_tile_count_helpers_reject_overflow();
    return 0;
}
