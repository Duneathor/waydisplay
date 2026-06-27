#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_dispatch.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

struct Expected {
    uint16_t type;
    uint32_t channels;
    uint32_t phases;
    uint32_t directions;
    wd_protocol_payload_kind kind;
    uint32_t prefix;
    uint32_t entry;
};

constexpr std::array<Expected, 28> ExpectedMessages{{
    {WD_MSG_CLIENT_HELLO, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_client_hello_payload), 0},
    {WD_MSG_SERVER_CONFIG, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION | WD_PROTOCOL_PHASE_ESTABLISHED,
     WD_PROTOCOL_SERVER_TO_CLIENT, WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_server_config_payload), 0},
    {WD_MSG_TILE_GENERATION_SUMMARY, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_REPEATED, sizeof(wd_tile_summary_payload_header), sizeof(wd_tile_generation_entry)},
    {WD_MSG_TILE_REPAIR_REQUEST, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_REPEATED, sizeof(wd_tile_repair_request_payload_header), sizeof(wd_tile_repair_entry)},
    {WD_MSG_KEYBOARD_KEY, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_keyboard_event_payload), 0},
    {WD_MSG_POINTER_EVENT, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_pointer_event_payload), 0},
    {WD_MSG_MTU_PROBE_START, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_mtu_probe_start_payload), 0},
    {WD_MSG_MTU_PROBE_RESULT, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_mtu_probe_result_payload), 0},
    {WD_MSG_CLIPBOARD_SET, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
     WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT, WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL,
     sizeof(wd_selection_payload_header), 0},
    {WD_MSG_CLIPBOARD_REQUEST, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
     WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT, WD_PROTOCOL_PAYLOAD_EMPTY, 0, 0},
    {WD_MSG_PRIMARY_SET, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
     WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT, WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL,
     sizeof(wd_selection_payload_header), 0},
    {WD_MSG_PRIMARY_REQUEST, WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
     WD_PROTOCOL_CLIENT_TO_SERVER | WD_PROTOCOL_SERVER_TO_CLIENT, WD_PROTOCOL_PAYLOAD_EMPTY, 0, 0},
    {WD_MSG_CURSOR_SHAPE, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_cursor_shape_payload), 0},
    {WD_MSG_DISPLAY_RESIZE, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_display_resize_payload), 0},
    {WD_MSG_THROUGHPUT_PROBE_START, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_throughput_probe_start_payload), 0},
    {WD_MSG_THROUGHPUT_PROBE_RESULT, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_throughput_probe_result_payload), 0},
    {WD_MSG_INPUT_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_input_channel_hello_payload), 0},
    {WD_MSG_SELECTION_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
     WD_PROTOCOL_CLIENT_TO_SERVER, WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_selection_channel_hello_payload), 0},
    {WD_MSG_CLIENT_STATS, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_client_stats_payload), 0},
    {WD_MSG_LINK_PROBE_PING, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION | WD_PROTOCOL_PHASE_ESTABLISHED,
     WD_PROTOCOL_SERVER_TO_CLIENT, WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_link_probe_payload), 0},
    {WD_MSG_LINK_PROBE_PONG, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_link_probe_payload), 0},
    {WD_MSG_VIDEO_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
     WD_PROTOCOL_CLIENT_TO_SERVER, WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_video_channel_hello_payload), 0},
    {WD_MSG_VIDEO_FRAME, WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL, sizeof(wd_video_frame_payload_header), 0},
    {WD_MSG_CONFIG_APPLIED, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_config_applied_payload), 0},
    {WD_MSG_AUDIO_CHANNEL_HELLO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE, WD_PROTOCOL_PHASE_NEGOTIATION,
     WD_PROTOCOL_CLIENT_TO_SERVER, WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_audio_channel_hello_payload), 0},
    {WD_MSG_AUDIO_CONFIG, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_audio_config_payload), 0},
    {WD_MSG_AUDIO_PACKET, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_SERVER_TO_CLIENT,
     WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL, sizeof(wd_audio_packet_payload_header), 0},
    {WD_MSG_VIDEO_FEEDBACK, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED, WD_PROTOCOL_CLIENT_TO_SERVER,
     WD_PROTOCOL_PAYLOAD_FIXED, sizeof(wd_video_feedback_payload), 0},
}};

void test_descriptor_completeness_and_sizes() {
    for (size_t index = 0; index < ExpectedMessages.size(); ++index)
    {
        const auto& expected = ExpectedMessages[index];
        CHECK(expected.type == index + 1);
        const auto* descriptor = wd_protocol_message_descriptor_find(expected.type);
        CHECK(descriptor != nullptr);
        CHECK(descriptor->message_type == expected.type);
        CHECK(descriptor->channels == expected.channels);
        CHECK(descriptor->phases == expected.phases);
        CHECK(descriptor->directions == expected.directions);
        CHECK(descriptor->payload_kind == expected.kind);
        CHECK(descriptor->payload_prefix_size == expected.prefix);
        CHECK(descriptor->repeated_entry_size == expected.entry);
        CHECK(wd_protocol_message_name(expected.type) != nullptr);

        switch (expected.kind)
        {
        case WD_PROTOCOL_PAYLOAD_EMPTY:
            CHECK(wd_protocol_payload_size_is_valid(expected.type, 0));
            CHECK(!wd_protocol_payload_size_is_valid(expected.type, 1));
            break;
        case WD_PROTOCOL_PAYLOAD_FIXED:
            CHECK(wd_protocol_payload_size_is_valid(expected.type, expected.prefix));
            if (expected.prefix > 0)
            {
                CHECK(!wd_protocol_payload_size_is_valid(expected.type, expected.prefix - 1));
            }
            CHECK(!wd_protocol_payload_size_is_valid(expected.type, expected.prefix + 1));
            break;
        case WD_PROTOCOL_PAYLOAD_OPAQUE_TAIL:
            CHECK(wd_protocol_payload_size_is_valid(expected.type, expected.prefix));
            CHECK(!wd_protocol_payload_size_is_valid(expected.type, expected.prefix - 1));
            CHECK(wd_protocol_payload_size_is_valid(expected.type, descriptor->maximum_payload_size));
            if (descriptor->maximum_payload_size != UINT32_MAX)
            {
                CHECK(!wd_protocol_payload_size_is_valid(expected.type, descriptor->maximum_payload_size + 1));
            }
            break;
        case WD_PROTOCOL_PAYLOAD_REPEATED:
            CHECK(wd_protocol_payload_size_is_valid(expected.type, expected.prefix));
            CHECK(wd_protocol_payload_size_is_valid(expected.type, expected.prefix + expected.entry));
            CHECK(!wd_protocol_payload_size_is_valid(expected.type, expected.prefix + 1));
            break;
        }
    }
    CHECK(wd_protocol_message_descriptor_find(0) == nullptr);
    CHECK(wd_protocol_message_descriptor_find(29) == nullptr);
    CHECK(!wd_protocol_payload_size_is_valid(29, 0));
}

void test_every_descriptor_has_an_allowed_route() {
    constexpr wd_protocol_channel channels[] = {
        WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_CHANNEL_SELECTION,
        WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE,
    };
    constexpr wd_protocol_phase phases[] = {WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_PHASE_ESTABLISHED};
    constexpr wd_protocol_direction directions[] = {WD_PROTOCOL_CLIENT_TO_SERVER, WD_PROTOCOL_SERVER_TO_CLIENT};

    for (const auto& expected : ExpectedMessages)
    {
        const auto* descriptor = wd_protocol_message_descriptor_find(expected.type);
        CHECK(descriptor != nullptr);
        unsigned allowed = 0;
        for (auto channel : channels)
        {
            for (auto phase : phases)
            {
                for (auto direction : directions)
                {
                    if (wd_protocol_message_allowed(expected.type, channel, phase, direction,
                                                    descriptor->minimum_payload_size))
                    {
                        ++allowed;
                        CHECK((expected.channels & channel) != 0);
                        CHECK((expected.phases & phase) != 0);
                        CHECK((expected.directions & direction) != 0);
                    }
                }
            }
        }
        CHECK(allowed != 0);
    }
}

void test_channel_caps_cover_all_allowed_messages() {
    constexpr wd_protocol_channel channels[] = {
        WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_CHANNEL_INPUT, WD_PROTOCOL_CHANNEL_SELECTION,
        WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_CHANNEL_AUX_HANDSHAKE,
    };
    constexpr wd_protocol_phase phases[] = {WD_PROTOCOL_PHASE_NEGOTIATION, WD_PROTOCOL_PHASE_ESTABLISHED};
    constexpr wd_protocol_direction directions[] = {WD_PROTOCOL_CLIENT_TO_SERVER, WD_PROTOCOL_SERVER_TO_CLIENT};
    for (auto channel : channels)
    {
        for (auto phase : phases)
        {
            for (auto direction : directions)
            {
                uint32_t expected_max = 0;
                for (const auto& expected : ExpectedMessages)
                {
                    const auto* descriptor = wd_protocol_message_descriptor_find(expected.type);
                    if ((descriptor->channels & channel) != 0 && (descriptor->phases & phase) != 0 &&
                        (descriptor->directions & direction) != 0 && descriptor->maximum_payload_size > expected_max)
                    {
                        expected_max = descriptor->maximum_payload_size;
                    }
                }
                CHECK(wd_protocol_channel_max_payload(channel, phase, direction) == expected_max);
            }
        }
    }
}

} // namespace

int main() {
    test_descriptor_completeness_and_sizes();
    test_every_descriptor_has_an_allowed_route();
    test_channel_caps_cover_all_allowed_messages();
    return 0;
}
