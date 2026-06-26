#include "client_config_validation.hpp"
#include "stream_ownership.h"
#include "video_transition.h"
#include "wd_video_transition.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

wd_server_config_payload base_config() {
    wd_server_config_payload config{};
    config.session_id         = 9;
    config.connection_token   = 0x1122334455667788ull;
    config.content_epoch      = 1;
    config.config_epoch       = 1;
    config.server_udp_port    = 5001;
    config.width              = 1664;
    config.height             = 1024;
    config.tile_width         = 16;
    config.tile_height        = 16;
    config.tiles_x            = 104;
    config.tiles_y            = 64;
    config.total_tiles        = 6656;
    config.pixel_format       = WD_PIXEL_FORMAT_XRGB8888;
    config.compression_mode   = WD_COMPRESSION_ZSTD;
    config.zstd_level         = 1;
    config.udp_payload_target = 1428;
    return config;
}

void stress_resize_keeps_transport_identity() {
    wd_server_config_payload current = base_config();
    std::mt19937             random(0x51a7e5u);

    for (uint64_t iteration = 0; iteration < 20000; ++iteration)
    {
        wd_server_config_payload next = current;
        next.width                    = static_cast<uint16_t>(640u + random() % (WD_MAX_RENDER_WIDTH - 639u));
        next.height                   = static_cast<uint16_t>(480u + random() % (WD_MAX_RENDER_HEIGHT - 479u));
        next.tiles_x                  = static_cast<uint16_t>((next.width + 15u) / 16u);
        next.tiles_y                  = static_cast<uint16_t>((next.height + 15u) / 16u);
        next.total_tiles              = static_cast<uint16_t>(static_cast<uint32_t>(next.tiles_x) * next.tiles_y);
        next.config_epoch++;
        next.content_epoch++;
        if (next.config_epoch == 0)
        {
            next.config_epoch = 1;
        }
        if (next.content_epoch == 0)
        {
            next.content_epoch = 1;
        }

        ClientConfigValidationError error{};
        require(client_normalize_and_validate_server_config(next, &error), "stress resize should remain a valid negotiated geometry");
        const uint32_t changes = client_classify_server_config_change(current, next);
        require((changes & ClientConfigChangeTransport) == 0, "ordinary resize must never recreate the transport");
        require((changes & ClientConfigChangeEpoch) != 0 && (changes & ClientConfigChangeContent) != 0,
                "ordinary resize must retain config and content barriers");
        current = next;
    }
}

void stress_video_transition_invariants() {
    std::mt19937          random(0x7a11e5u);
    enum wd_client_video_phase         phase     = WD_CLIENT_VIDEO_PHASE_TILES;
    struct wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;

    for (uint64_t iteration = 0; iteration < 100000; ++iteration)
    {
        const bool resize   = (random() % 97u) == 0;
        const bool eos      = resize || (random() % 83u) == 0;
        const bool advanced = !eos && (random() % 41u) == 0;
        const bool keyframe = (random() % 17u) == 0;
        const bool payload  = !eos && (random() % 5u) != 0;

        const struct wd_client_video_transition_decision decision =
            wd_client_video_transition_decide(phase, advanced, eos, resize, keyframe, payload);

        require(!decision.accept_payload || payload, "transition cannot accept an empty video payload");
        require(!decision.accept_payload || keyframe || phase == WD_CLIENT_VIDEO_PHASE_VIDEO,
                "video entry cannot accept an inter-frame before a keyframe");
        if (resize)
        {
            require(decision.next_phase == WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME, "resize must leave the decoder waiting for a keyframe");
        }
        else if (eos)
        {
            require(decision.next_phase == WD_CLIENT_VIDEO_PHASE_TILES, "EOS must return to tile ownership");
        }

        phase = decision.next_phase;
        if (phase == WD_CLIENT_VIDEO_PHASE_VIDEO)
        {
            const uint64_t epoch = wd_client_stream_ownership_begin_video_stream(&ownership);
            const struct wd_client_content_ownership_snapshot snapshot =
                wd_client_stream_ownership_snapshot(&ownership);
            require(snapshot.epoch == epoch && snapshot.owner == WD_CLIENT_CONTENT_OWNER_VIDEO &&
                        wd_client_stream_ownership_is_current(&ownership, snapshot.epoch, WD_CLIENT_CONTENT_OWNER_VIDEO),
                    "video ownership snapshot must remain current");
        }
        else if (phase == WD_CLIENT_VIDEO_PHASE_TILES)
        {
            const uint64_t epoch = wd_client_stream_ownership_end_video_stream(&ownership);
            require(wd_client_stream_ownership_is_current(&ownership, epoch, WD_CLIENT_CONTENT_OWNER_TILES),
                    "tile ownership snapshot must remain current");
        }
    }
}

void stress_server_keyframe_epoch_commit() {
    uint64_t epoch = 1;
    for (uint64_t iteration = 0; iteration < 100000; ++iteration)
    {
        const wd_video_entry_plan plan = wd_video_entry_plan_make(epoch, true, true);
        require(plan.source_content_epoch == epoch && plan.commit_on_queue, "entry plan should reserve from the current epoch");
        require(!wd_video_entry_plan_can_commit(&plan, epoch, false), "failed queue cannot commit a reserved epoch");
        require(wd_video_entry_plan_can_commit(&plan, epoch, true), "successful queue should commit the reserved keyframe epoch");
        epoch = plan.frame_content_epoch;
    }
}

} // namespace

int main() {
    stress_resize_keeps_transport_identity();
    stress_video_transition_invariants();
    stress_server_keyframe_epoch_commit();
    return 0;
}
