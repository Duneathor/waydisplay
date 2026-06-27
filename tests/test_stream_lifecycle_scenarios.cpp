#include "audio_video_sync.h"
#include "stream_ownership.h"
#include "wd_connection_identity.h"
#include "wd_video_transition.h"

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

enum class Owner { Tiles, Video };

struct Scenario {
    uint8_t session_id = 0;
    uint64_t connection_epoch = 0;
    uint64_t content_epoch = 0;
    uint64_t tile_presented_epoch = 0;
    uint64_t bootstrap_epoch = 0;
    uint64_t recovery_epoch = 0;
    uint32_t cooldown_seconds = 0;
    bool connected = false;
    bool forced = false;
    bool recovery_active = false;
    bool recovery_refresh_sent = false;
    wd_video_recovery_class recovery_class = WD_VIDEO_RECOVERY_NONE;
    Owner owner = Owner::Tiles;
    wd_client_stream_ownership client_ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;

    void connect(bool force_video) {
        connected = true;
        forced = force_video;
        session_id = wd_connection_next_session_id(session_id);
        connection_epoch = wd_next_nonzero_epoch(connection_epoch);
        content_epoch = wd_next_nonzero_epoch(content_epoch);
        tile_presented_epoch = 0;
        bootstrap_epoch = content_epoch;
        recovery_epoch = 0;
        recovery_active = false;
        recovery_refresh_sent = false;
        recovery_class = WD_VIDEO_RECOVERY_NONE;
        cooldown_seconds = 0;
        owner = Owner::Tiles;
        wd_client_stream_ownership_reset_to_tiles(&client_ownership);
    }

    void disconnect() {
        connected = false;
        recovery_active = false;
        recovery_refresh_sent = false;
        recovery_class = WD_VIDEO_RECOVERY_NONE;
        owner = Owner::Tiles;
    }

    bool bootstrap_pending() const {
        return bootstrap_epoch != 0 && tile_presented_epoch < bootstrap_epoch;
    }

    bool may_enter_video() const {
        return connected && wd_video_entry_allowed(bootstrap_pending(), recovery_active, cooldown_seconds, forced, recovery_class);
    }

    void present_tiles(uint64_t epoch) {
        if (epoch > tile_presented_epoch)
        {
            tile_presented_epoch = epoch;
        }
        if (bootstrap_epoch != 0 && tile_presented_epoch >= bootstrap_epoch)
        {
            bootstrap_epoch = 0;
        }
        if (recovery_active &&
            wd_tile_recovery_decide(recovery_refresh_sent, recovery_epoch, tile_presented_epoch, 0, 5) ==
                WD_TILE_RECOVERY_COMPLETE_PRESENTED)
        {
            recovery_active = false;
            owner = Owner::Tiles;
            cooldown_seconds = recovery_class == WD_VIDEO_RECOVERY_FAILURE ? 5 : 0;
        }
    }

    void enter_video() {
        CHECK(may_enter_video());
        const wd_video_entry_plan plan = wd_video_entry_plan_make(content_epoch, true, true);
        CHECK(wd_video_entry_plan_can_commit(&plan, content_epoch, true));
        content_epoch = plan.frame_content_epoch;
        owner = Owner::Video;
        recovery_class = WD_VIDEO_RECOVERY_NONE;
        wd_client_stream_ownership_begin_video_stream(&client_ownership);
    }

    void begin_recovery(wd_video_recovery_class kind) {
        CHECK(owner == Owner::Video || kind == WD_VIDEO_RECOVERY_PLANNED);
        content_epoch = wd_next_nonzero_epoch(content_epoch);
        recovery_epoch = content_epoch;
        recovery_active = true;
        recovery_refresh_sent = true;
        recovery_class = kind;
        owner = Owner::Tiles;
        wd_client_stream_ownership_end_video_stream(&client_ownership);
    }
};

void test_first_forced_connection_requires_bootstrap() {
    Scenario s;
    s.connect(true);
    CHECK(s.session_id != 0 && s.connection_epoch != 0);
    CHECK(s.bootstrap_pending());
    CHECK(!s.may_enter_video());
    s.present_tiles(s.content_epoch - 1);
    CHECK(!s.may_enter_video());
    s.present_tiles(s.content_epoch);
    CHECK(s.may_enter_video());
    s.enter_video();
    CHECK(s.owner == Owner::Video);
}

void test_tile_force_and_force_tile_reconnects_are_isolated() {
    Scenario s;
    s.connect(false);
    const uint8_t tile_session = s.session_id;
    const uint64_t tile_connection = s.connection_epoch;
    s.present_tiles(s.content_epoch);
    CHECK(s.may_enter_video());
    s.disconnect();

    s.connect(true);
    CHECK(s.session_id != tile_session && s.connection_epoch != tile_connection);
    CHECK(s.bootstrap_pending());
    /* A delayed acknowledgment from the previous connection cannot satisfy the new epoch. */
    s.present_tiles(s.content_epoch - 1);
    CHECK(s.bootstrap_pending() && !s.may_enter_video());
    s.present_tiles(s.content_epoch);
    s.enter_video();
    s.disconnect();

    s.connect(false);
    CHECK(s.owner == Owner::Tiles && s.bootstrap_pending());
    CHECK(wd_client_stream_ownership_snapshot(&s.client_ownership).owner == WD_CLIENT_CONTENT_OWNER_TILES);
    s.present_tiles(s.content_epoch);
    CHECK(!s.bootstrap_pending());
}

void test_forced_resize_uses_exact_planned_recovery_ack() {
    Scenario s;
    s.connect(true);
    s.present_tiles(s.content_epoch);
    s.enter_video();
    const uint64_t old_video_epoch = s.content_epoch;
    s.begin_recovery(WD_VIDEO_RECOVERY_PLANNED);
    CHECK(s.recovery_epoch > old_video_epoch);
    CHECK(!s.may_enter_video());
    s.present_tiles(old_video_epoch);
    CHECK(s.recovery_active && !s.may_enter_video());
    s.present_tiles(s.recovery_epoch);
    CHECK(!s.recovery_active && s.cooldown_seconds == 0);
    CHECK(s.may_enter_video());
    s.enter_video();
}

void test_video_failure_has_circuit_breaker() {
    Scenario s;
    s.connect(true);
    s.present_tiles(s.content_epoch);
    s.enter_video();
    s.begin_recovery(WD_VIDEO_RECOVERY_FAILURE);
    s.present_tiles(s.recovery_epoch);
    CHECK(!s.recovery_active);
    CHECK(s.cooldown_seconds == 5);
    CHECK(!s.may_enter_video());
    s.cooldown_seconds = 0;
    CHECK(s.may_enter_video());
}

void test_audio_never_starts_cannot_hold_video_forever() {
    CHECK(wd_client_audio_startup_gate_decide(true, false, true, 0, 1000) == WD_CLIENT_AUDIO_STARTUP_HOLD);
    CHECK(wd_client_audio_startup_gate_decide(true, false, true, 999, 1000) == WD_CLIENT_AUDIO_STARTUP_HOLD);
    CHECK(wd_client_audio_startup_gate_decide(true, false, true, 1000, 1000) == WD_CLIENT_AUDIO_STARTUP_TIMEOUT);
    CHECK(wd_client_audio_startup_gate_decide(true, true, true, 0, 1000) == WD_CLIENT_AUDIO_STARTUP_READY);
}

void test_client_ownership_epochs_reject_stale_work() {
    wd_client_stream_ownership ownership = WD_CLIENT_STREAM_OWNERSHIP_INITIALIZER;
    const auto initial = wd_client_stream_ownership_snapshot(&ownership);
    const uint64_t video_epoch = wd_client_stream_ownership_begin_video_stream(&ownership);
    CHECK(video_epoch > initial.epoch);
    CHECK(!wd_client_stream_ownership_is_current(&ownership, initial.epoch, initial.owner));
    const uint64_t tile_epoch = wd_client_stream_ownership_end_video_stream(&ownership);
    CHECK(tile_epoch > video_epoch);
    CHECK(!wd_client_stream_ownership_is_current(&ownership, video_epoch, WD_CLIENT_CONTENT_OWNER_VIDEO));
    CHECK(wd_client_stream_ownership_is_current(&ownership, tile_epoch, WD_CLIENT_CONTENT_OWNER_TILES));
}

} // namespace

int main() {
    test_first_forced_connection_requires_bootstrap();
    test_tile_force_and_force_tile_reconnects_are_isolated();
    test_forced_resize_uses_exact_planned_recovery_ack();
    test_video_failure_has_circuit_breaker();
    test_audio_never_starts_cannot_hold_video_forever();
    test_client_ownership_epochs_reject_stale_work();
    return 0;
}
