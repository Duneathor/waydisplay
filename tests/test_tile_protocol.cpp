#include "waydisplay/wd_protocol.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
    require(WD_PROTOCOL_VERSION == 37, "config-epoch protocol should bump the wire version");
    require(WD_UDP_TILE_HEADER_MIN_SIZE == 36, "canonical tile header size");
    require(WD_UDP_TILE_HEADER_MAX_SIZE == 44, "correlated tile header size");
}

void test_tile_size_round_trip() {
    struct Case { uint16_t width; uint16_t height; uint8_t code; };
    const Case cases[] = {{128, 64, WD_TILE_128x64}, {64, 64, WD_TILE_64x64},
                          {32, 32, WD_TILE_32x32}, {16, 16, WD_TILE_16x16}};
    for (const Case& item : cases)
    {
        uint8_t code = 0xff;
        require(wd_tile_size_code_for_dimensions(item.width, item.height, &code), "dimension-to-code mapping");
        require(code == item.code, "dimension-to-code value");
        uint16_t width = 0;
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
    source.session_id = 7;
    source.connection_token = 0x1122334455667788ull;
    source.content_epoch = 5;
    source.flags = WD_UDP_TILE_FLAG_COMPRESSED;
    source.tile_size = WD_TILE_32x32;
    source.tile_pkt_id = 1;
    source.tile_id = 9;
    source.tile_pkt_count = 3;
    source.payload_size = 13;
    source.tile_payload_size = 29;
    source.tile_generation = 42;

    const auto packet = encode_packet(source);
    wd_udp_tile_packet_decoded decoded{};
    require(wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "base header should decode");
    require(decoded.session_id == source.session_id && decoded.connection_token == source.connection_token && decoded.content_epoch == source.content_epoch && decoded.flags == source.flags, "base identity fields");
    require(decoded.tile_size == source.tile_size && decoded.tile_id == source.tile_id, "base tile fields");
    require(decoded.tile_pkt_count == 3 && decoded.tile_pkt_id == 1, "base fragment fields");
    require(decoded.payload_size == 13 && decoded.tile_payload_size == 29, "base payload fields");
    require(decoded.tile_generation == 42 && decoded.input_sequence == 0, "base generation fields");
    require(decoded.header_size == WD_UDP_TILE_HEADER_MIN_SIZE, "base header decoded size");
    require(wd_udp_tile_packet_is_compressed(&decoded), "compressed flag should decode");
}

void test_input_extension_round_trip() {
    wd_udp_tile_packet_decoded source{};
    source.session_id = 3;
    source.connection_token = 0x8877665544332211ull;
    source.content_epoch = 5;
    source.flags = WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
    source.tile_size = WD_TILE_16x16;
    source.tile_pkt_id = 0;
    source.tile_id = 4;
    source.tile_pkt_count = 2;
    source.payload_size = 400;
    source.tile_payload_size = 700;
    source.tile_generation = 12;
    source.input_sequence = 777;

    const auto packet = encode_packet(source);
    wd_udp_tile_packet_decoded decoded{};
    require(wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "extended header should decode");
    require(decoded.header_size == WD_UDP_TILE_HEADER_MAX_SIZE, "extension should enlarge only packet zero");
    require(decoded.input_sequence == 777, "input sequence should round trip");
    require(!wd_udp_tile_packet_is_compressed(&decoded), "uncompressed flag should decode");
}

void test_rejects_invalid_extensions_and_flags() {
    wd_udp_tile_packet_decoded header{};
    header.session_id = 1;
    header.connection_token = 9;
    header.content_epoch = 5;
    header.flags = WD_UDP_TILE_FLAG_INPUT_SEQUENCE;
    header.tile_size = WD_TILE_16x16;
    header.tile_pkt_id = 1;
    header.tile_id = 0;
    header.tile_pkt_count = 2;
    header.payload_size = 10;
    header.tile_payload_size = 20;
    header.tile_generation = 1;
    header.input_sequence = 9;
    std::array<uint8_t, WD_UDP_TILE_HEADER_MAX_SIZE> encoded{};
    require(!wd_udp_tile_packet_encode_header(encoded.data(), encoded.size(), &header),
            "input extension should be legal only on packet zero");

    header.tile_pkt_id = 0;
    header.flags = 0x80;
    require(!wd_udp_tile_packet_encode_header(encoded.data(), encoded.size(), &header), "unknown flags should be rejected");

    wd_udp_tile_packet_header wire{};
    wire.session_id = 1;
    wire.connection_token = 9;
    wire.content_epoch = 5;
    wire.flags = 0x80;
    wire.tile_pkt_count = 1;
    wire.payload_size = 1;
    wire.tile_payload_size = 1;
    wire.tile_generation = 1;
    std::array<uint8_t, sizeof(wire) + 1> packet{};
    std::memcpy(packet.data(), &wire, sizeof(wire));
    wd_udp_tile_packet_decoded decoded{};
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "unknown wire flags should be rejected");

    wire.flags = 0;
    wire.connection_token = 0;
    wire.content_epoch = 5;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "zero connection token should be rejected");

    wire.connection_token = 9;
    wire.content_epoch = 0;
    std::memcpy(packet.data(), &wire, sizeof(wire));
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "zero content epoch should be rejected");
}

void test_decode_rejects_trailing_bytes_and_reserved_data() {
    wd_udp_tile_packet_decoded header{};
    header.session_id = 1;
    header.connection_token = 9;
    header.content_epoch = 5;
    header.tile_size = WD_TILE_16x16;
    header.tile_pkt_count = 1;
    header.payload_size = 4;
    header.tile_payload_size = 4;
    header.tile_generation = 1;
    auto packet = encode_packet(header);
    packet.push_back(0);
    wd_udp_tile_packet_decoded decoded{};
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "trailing bytes should be rejected");

    packet.pop_back();
    auto* wire = reinterpret_cast<wd_udp_tile_packet_header*>(packet.data());
    wire->reserved = 1;
    require(!wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded), "reserved byte should be zero");
}

void test_fragment_layout_is_canonical() {
    wd_udp_tile_packet_decoded decoded{};
    decoded.header_size = WD_UDP_TILE_HEADER_MIN_SIZE;
    decoded.tile_payload_size = 1024;
    decoded.tile_pkt_count = 3;
    decoded.tile_pkt_id = 0;
    decoded.payload_size = 400;
    require(wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "first full fragment should be valid");
    decoded.tile_pkt_id = 2;
    decoded.payload_size = 224;
    require(wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "final remainder fragment should be valid");
    decoded.payload_size = 223;
    require(!wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "short final fragment should be rejected");
    decoded.payload_size = 224;
    decoded.tile_pkt_count = 4;
    require(!wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "noncanonical packet count should be rejected");
    decoded.tile_pkt_count = 3;
    decoded.tile_payload_size = 1023;
    require(!wd_udp_tile_fragment_layout_valid(&decoded, decoded.header_size + decoded.payload_size, 400, 1024),
            "declared total payload must match validated payload");
}


void test_protocol_v37_strict_payload_helpers() {
    require(sizeof(wd_server_config_payload) == 73, "v37 server config should include config epoch");
    require(sizeof(wd_config_applied_payload) == 17, "v37 config ACK should include config epoch");
    require(wd_fixed_payload_size_is_valid(17, sizeof(wd_config_applied_payload)),
            "fixed payload helper should accept exact size");
    require(!wd_fixed_payload_size_is_valid(18, sizeof(wd_config_applied_payload)),
            "fixed payload helper should reject trailing bytes");
    wd_config_applied_payload applied{4, 5, 6};
    require(wd_config_applied_matches(&applied, 4, 5, 6),
            "config ACK should match the exact connection and config epoch");
    require(!wd_config_applied_matches(&applied, 4, 5, 7),
            "stale config ACK epoch should be rejected");
    require(wd_counted_payload_size_is_valid(28 + 20, 28, 2, 10),
            "counted payload helper should accept exact element count");
    require(!wd_counted_payload_size_is_valid(28 + 21, 28, 2, 10),
            "counted payload helper should reject trailing bytes");
    require(wd_tile_summary_count_is_valid(0, 64, 64),
            "full summaries should cover every configured tile");
    require(!wd_tile_summary_count_is_valid(0, 63, 64),
            "short full summaries should be rejected");
    require(wd_tile_summary_count_is_valid(WD_TILE_SUMMARY_DELTA, 0, 64),
            "empty delta summaries should remain canonical");
    require(!wd_tile_summary_count_is_valid(WD_TILE_SUMMARY_DELTA, 65, 64),
            "summary counts cannot exceed the configured tile count");
    require(wd_tile_repair_count_is_valid(1, 64),
            "nonempty bounded repair requests should validate");
    require(!wd_tile_repair_count_is_valid(0, 64),
            "empty repair requests should be rejected");
    require(!wd_tile_repair_count_is_valid(65, 64),
            "repair requests cannot exceed the configured tile count");
}

void test_client_hello_strict_validation() {
    wd_client_hello_payload hello{};
    hello.client_udp_port = 6000;
    hello.video_mode = WD_VIDEO_MODE_AUTO;
    require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)),
            "non-video client hello should validate");

    hello.capabilities = WD_CLIENT_CAP_VIDEO_STREAM;
    hello.video_codecs = WD_VIDEO_CODEC_H265;
    hello.video_transport = WD_VIDEO_TRANSPORT_TCP;
    require(wd_client_hello_payload_is_valid(&hello, sizeof(hello)),
            "canonical video client hello should validate");

    hello.video_reserved = 1;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)),
            "reserved client hello bits should be rejected");
    hello.video_reserved = 0;
    hello.capabilities |= 0x80000000u;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello)),
            "unknown client capabilities should be rejected");
    hello.capabilities = WD_CLIENT_CAP_VIDEO_STREAM;
    require(!wd_client_hello_payload_is_valid(&hello, sizeof(hello) + 1),
            "client hello trailing bytes should be rejected");
}

void test_video_frame_strict_validation() {
    wd_video_frame_payload_header frame{};
    frame.session_id = 1;
    frame.connection_token = 2;
    frame.content_epoch = 3;
    frame.codec = WD_VIDEO_CODEC_H265;
    frame.flags = WD_VIDEO_FRAME_KEYFRAME | WD_VIDEO_FRAME_CONFIG;
    frame.frame_id = 1;
    frame.width = 1280;
    frame.height = 720;
    frame.coded_width = 1280;
    frame.coded_height = 720;
    frame.data_size = 4;
    require(wd_video_frame_payload_size_is_valid(&frame, sizeof(frame) + 4),
            "canonical video data frame should validate");

    frame.flags |= 0x8000u;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame) + 4),
            "unknown video flags should be rejected");
    frame.flags = WD_VIDEO_FRAME_CONFIG;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame) + 4),
            "config video frames must also be keyframes");

    frame.flags = WD_VIDEO_FRAME_END_OF_STREAM;
    frame.data_size = 0;
    require(wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)),
            "canonical video EOS should validate");
    frame.flags |= WD_VIDEO_FRAME_KEYFRAME;
    require(!wd_video_frame_payload_size_is_valid(&frame, sizeof(frame)),
            "control frames cannot also be keyframes");
}


void test_input_payload_strict_validation() {
    wd_keyboard_event_payload key{};
    key.session_id = 1;
    key.connection_token = 2;
    key.client_timestamp_ns = 3;
    key.input_sequence = 4;
    key.evdev_key_code = 30;
    key.pressed = 1;
    require(wd_keyboard_event_payload_is_valid(&key, sizeof(key)),
            "canonical keyboard payload should validate");
    key.pressed = 2;
    require(!wd_keyboard_event_payload_is_valid(&key, sizeof(key)),
            "invalid keyboard state should be rejected");

    wd_pointer_event_payload pointer{};
    pointer.session_id = 1;
    pointer.connection_token = 2;
    pointer.client_timestamp_ns = 3;
    pointer.input_sequence = 4;
    pointer.event_type = WD_POINTER_EVENT_AXIS;
    pointer.axis = WD_POINTER_AXIS_VERTICAL;
    pointer.axis_value = -120;
    require(wd_pointer_event_payload_is_valid(&pointer, sizeof(pointer)),
            "canonical pointer axis payload should validate");
    pointer.axis_value = 0;
    require(!wd_pointer_event_payload_is_valid(&pointer, sizeof(pointer)),
            "zero-value pointer axis payload should be rejected");
    pointer.axis_value = -120;
    pointer.modifiers = 0x10;
    require(!wd_pointer_event_payload_is_valid(&pointer, sizeof(pointer)),
            "unknown pointer modifier bits should be rejected");
}

} // namespace

int main() {
    test_protocol_version_and_header_sizes();
    test_tile_size_round_trip();
    test_base_header_round_trip();
    test_input_extension_round_trip();
    test_rejects_invalid_extensions_and_flags();
    test_decode_rejects_trailing_bytes_and_reserved_data();
    test_fragment_layout_is_canonical();
    test_protocol_v37_strict_payload_helpers();
    test_client_hello_strict_validation();
    test_video_frame_strict_validation();
    test_input_payload_strict_validation();
    return 0;
}
