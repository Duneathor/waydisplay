#include "wd_tile_policy.h"
#include "wd_video_transition.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void test_wire_bytes_account_for_one_extended_header() {
    require(wd_tile_wire_bytes_for_payload(1000, 400, 20, 28) == 1068, "three packets should include one extended and two base headers");
    require(wd_tile_wire_bytes_for_payload(400, 400, 20, 28) == 428, "single packet should use the first-packet header size");
}

void test_auto_video_entry_uses_sustained_wire_cost() {
    wd_video_auto_entry_metrics metrics{};
    metrics.frame_samples                 = 1048;
    metrics.changed_frame_samples         = 753;
    metrics.dirty_coverage_per_mille_sum  = 200168;
    metrics.dirty_coverage_per_mille_peak = 1000;
    metrics.tile_wire_bytes               = 9u * 1024u * 1024u;
    metrics.tile_budget_bytes_per_second  = 10u * 1024u * 1024u;
    metrics.requested_capture_fps         = 120;
    metrics.adaptive_capture_fps          = 120;
    metrics.minimum_dirty_percent         = 60;

    const wd_video_auto_entry_result video = wd_video_auto_entry_evaluate(&metrics);
    require(video.candidate, "sustained partial-screen video near the tile budget should enter video mode");
    metrics.selection_suppressed               = true;
    const wd_video_auto_entry_result bootstrap = wd_video_auto_entry_evaluate(&metrics);
    require(!bootstrap.candidate, "bootstrap and recovery refresh traffic must not select video mode");
    metrics.selection_suppressed = false;
    require(video.changed_frame_percent >= 70 && video.average_dirty_percent >= 19,
            "classifier should expose changed-frame frequency and all-frame dirty coverage");

    metrics.changed_frame_samples          = 2;
    metrics.dirty_coverage_per_mille_sum   = 2000;
    const wd_video_auto_entry_result burst = wd_video_auto_entry_evaluate(&metrics);
    require(!burst.candidate, "isolated full-screen bursts should not look like video");

    metrics.frame_samples                  = 60;
    metrics.changed_frame_samples          = 30;
    metrics.dirty_coverage_per_mille_sum   = 9000;
    metrics.dirty_coverage_per_mille_peak  = 400;
    metrics.tile_wire_bytes                = 1024;
    metrics.tile_budget_bytes_per_second   = 10u * 1024u * 1024u;
    const wd_video_auto_entry_result cheap = wd_video_auto_entry_evaluate(&metrics);
    require(!cheap.candidate, "cheap small animations should stay on tiles");

    metrics.dirty_coverage_per_mille_sum        = 36000;
    metrics.dirty_coverage_per_mille_peak       = 900;
    metrics.adaptive_capture_fps                = 60;
    const wd_video_auto_entry_result suppressed = wd_video_auto_entry_evaluate(&metrics);
    require(suppressed.candidate, "capture suppression should be a usable cost signal");

    metrics.frame_samples = 60;
    metrics.changed_frame_samples = 60;
    metrics.dirty_coverage_per_mille_sum = 30000; // 50% average across all frames.
    metrics.dirty_coverage_per_mille_peak = 500;
    metrics.tile_wire_bytes = 0;
    metrics.estimated_tile_demand_bytes_per_second = 0;
    metrics.adaptive_capture_fps = 120;
    metrics.minimum_dirty_percent = 50;
    require(wd_video_auto_entry_evaluate(&metrics).candidate,
            "sustained 50 percent all-frame turnover should favor video without waiting for congestion");

    metrics.dirty_coverage_per_mille_sum = 18000; // 30% average.
    metrics.estimated_tile_demand_bytes_per_second = 9u * 1024u * 1024u;
    metrics.tile_budget_bytes_per_second = 10u * 1024u * 1024u;
    require(wd_video_auto_entry_evaluate(&metrics).candidate,
            "predicted tile demand above 85 percent should favor video below the content threshold");
}

void test_tile_demand_estimate_uses_all_frame_dirty_average() {
    const uint64_t demand = wd_tile_estimate_demand_bytes_per_second(
        60, 30000, 60000, 60, 1900, 120, 1100);
    require(demand == 114000000u, "demand should scale average wire bytes by dirty coverage, tile count, and requested FPS");
    require(wd_tile_estimate_demand_bytes_per_second(0, 0, 0, 0, 1900, 120, 1100) == 0,
            "missing frame samples should produce no demand estimate");
}

void test_video_control_selection_is_authoritative() {
    require(!wd_video_control_allows_entry(WD_VIDEO_MODE_OFF, true, true, true),
            "video off must block entry even when every transport prerequisite is ready");
    require(!wd_video_control_allows_entry(WD_VIDEO_MODE_FORCE, false, true, true),
            "forced video must still require successful negotiation");
    require(!wd_video_control_allows_entry(WD_VIDEO_MODE_FORCE, true, false, true),
            "forced video must still require the video channel");
    require(!wd_video_control_allows_entry(WD_VIDEO_MODE_FORCE, true, true, false),
            "forced video must still require an encoder");
    require(wd_video_control_allows_entry(WD_VIDEO_MODE_FORCE, true, true, true),
            "forced video may bypass content thresholds only after control prerequisites are satisfied");
}

void test_compression_requires_material_savings() {
    require(!wd_tile_compression_is_worthwhile(980, 1000, 400, 20, 20, 64, 3), "minor byte savings should not pay compression cost");
    require(wd_tile_compression_is_worthwhile(700, 1000, 400, 20, 20, 64, 3), "material wire savings should select compression");
    require(!wd_tile_compression_is_worthwhile(1000, 1000, 400, 20, 20, 0, 0), "ties should prefer uncompressed payloads");
}

void test_locality_with_starvation_bound() {
    const std::array<uint16_t, 4> ids{0, 7, 9, 15};
    std::array<uint64_t, 16>      queued{};
    queued[0]  = 900;
    queued[7]  = 800;
    queued[9]  = 700;
    queued[15] = 100;
    require(wd_tile_select_local_region_index(ids.data(), ids.size(), 4, 8, queued.data(), queued.size(), 1000, 0) == 2,
            "nearest region should win without starvation");
    require(wd_tile_select_local_region_index(ids.data(), ids.size(), 4, 8, queued.data(), queued.size(), 1000, 500) == 3,
            "oldest region should win once starvation threshold is reached");
}

void test_xrgb_compression_prefilter() {
    std::vector<uint32_t> flat(256, 0x00112233u);
    require(wd_tile_xrgb_payload_may_compress(reinterpret_cast<const uint8_t*>(flat.data()),
                                              static_cast<uint32_t>(flat.size() * sizeof(uint32_t))),
            "flat tiles should reach the compressor");

    std::vector<uint32_t> gradient(256);
    for (uint32_t i = 0; i < gradient.size(); ++i)
    {
        gradient[i] = i;
    }
    require(wd_tile_xrgb_payload_may_compress(reinterpret_cast<const uint8_t*>(gradient.data()),
                                              static_cast<uint32_t>(gradient.size() * sizeof(uint32_t))),
            "regular gradients should reach the compressor");

    std::vector<uint32_t> noise(256);
    uint32_t              state = 0x12345678u;
    for (uint32_t& pixel : noise)
    {
        state = state * 1664525u + 1013904223u;
        pixel = state & 0x00ffffffu;
    }
    require(!wd_tile_xrgb_payload_may_compress(reinterpret_cast<const uint8_t*>(noise.data()),
                                               static_cast<uint32_t>(noise.size() * sizeof(uint32_t))),
            "high-entropy tiles should bypass zstd");
}

void test_compression_benchmark_modes() {
    uint8_t mode = 255;
    require(wd_tile_compression_benchmark_mode_parse("auto", &mode) && mode == WD_TILE_COMPRESSION_BENCH_AUTO,
            "auto benchmark mode should parse");
    require(wd_tile_compression_benchmark_mode_parse("off", &mode) && mode == WD_TILE_COMPRESSION_BENCH_OFF,
            "off benchmark mode should parse");
    require(wd_tile_compression_benchmark_mode_parse("attempt", &mode) && mode == WD_TILE_COMPRESSION_BENCH_ATTEMPT,
            "attempt benchmark mode should parse");
    require(wd_tile_compression_benchmark_mode_parse("force", &mode) && mode == WD_TILE_COMPRESSION_BENCH_FORCE,
            "force benchmark mode should parse");
    require(!wd_tile_compression_benchmark_mode_parse("invalid", &mode), "unknown benchmark modes should be rejected");

    require(!wd_tile_compression_benchmark_should_attempt(WD_TILE_COMPRESSION_BENCH_AUTO, false, true),
            "auto should respect the entropy prefilter");
    require(!wd_tile_compression_benchmark_should_attempt(WD_TILE_COMPRESSION_BENCH_AUTO, true, false),
            "auto should respect adaptive backoff");
    require(!wd_tile_compression_benchmark_should_attempt(WD_TILE_COMPRESSION_BENCH_OFF, true, true),
            "off should skip every compression attempt");
    require(wd_tile_compression_benchmark_should_attempt(WD_TILE_COMPRESSION_BENCH_ATTEMPT, false, false),
            "attempt should bypass prefilters for measurement");
    require(wd_tile_compression_benchmark_should_attempt(WD_TILE_COMPRESSION_BENCH_FORCE, false, false),
            "force should bypass prefilters for measurement");

    require(!wd_tile_compression_benchmark_choose_compressed(WD_TILE_COMPRESSION_BENCH_ATTEMPT, true, false),
            "attempt should still choose the smaller wire representation");
    require(wd_tile_compression_benchmark_choose_compressed(WD_TILE_COMPRESSION_BENCH_FORCE, true, false),
            "force should select any successful compressed result");
    require(!wd_tile_compression_benchmark_choose_compressed(WD_TILE_COMPRESSION_BENCH_FORCE, false, true),
            "force cannot select a failed compression result");
    require(std::string(wd_tile_compression_benchmark_mode_name(WD_TILE_COMPRESSION_BENCH_ATTEMPT)) == "attempt",
            "benchmark mode names should be stable for logs");
}

void test_compression_advisor_backs_off_and_resamples() {
    wd_tile_compression_advisor advisor{};
    for (int i = 0; i < 8; ++i)
    {
        require(wd_tile_compression_advisor_should_attempt(&advisor), "advisor should initially sample every candidate");
        wd_tile_compression_advisor_record(&advisor, false);
    }
    uint32_t attempts = 0;
    for (int i = 0; i < 32; ++i)
    {
        if (wd_tile_compression_advisor_should_attempt(&advisor))
        {
            attempts++;
        }
    }
    require(attempts == 2, "backoff should periodically resample rather than disable compression permanently");
    wd_tile_compression_advisor_record(&advisor, true);
    require(wd_tile_compression_advisor_should_attempt(&advisor), "a successful sample should immediately restore normal attempts");
}

void test_delivery_status_waits_for_seal_and_reports_failure() {
    wd_tile_delivery_status status{};
    wd_tile_delivery_status_add(&status);
    wd_tile_delivery_status_add(&status);
    bool failed = false;
    require(!wd_tile_delivery_status_complete(&status, true, &failed), "completion before seal should not finalize a tile");
    require(!wd_tile_delivery_status_seal(&status, &failed), "sealed tile should wait for remaining packets");
    require(wd_tile_delivery_status_complete(&status, false, &failed), "last packet should finalize a sealed tile");
    require(failed, "any failed packet should fail the tile delivery");
}

void test_tile_recovery_waits_for_client_presentation() {
    require(wd_tile_recovery_decide(false, 9, 9, 20, 5) == WD_TILE_RECOVERY_WAIT, "recovery cannot finish before the full refresh is sent");
    require(wd_tile_recovery_decide(true, 9, 9, 0, 5) == WD_TILE_RECOVERY_COMPLETE_PRESENTED,
            "client tile presentation should complete recovery");
    require(wd_tile_recovery_decide(true, 9, 8, 4, 5) == WD_TILE_RECOVERY_WAIT, "recovery should remain sticky before its timeout");
    require(wd_tile_recovery_decide(true, 9, 8, 5, 5) == WD_TILE_RECOVERY_COMPLETE_TIMEOUT,
            "recovery should have a bounded acknowledgement timeout");
    require(!wd_video_entry_allowed(false, true, 0, true, WD_VIDEO_RECOVERY_PLANNED), "video entry must be blocked while tile recovery is active");
    require(!wd_video_entry_allowed(false, false, 1, false, WD_VIDEO_RECOVERY_PLANNED), "video entry must respect the post-recovery cooldown");
    require(wd_video_entry_allowed(false, false, 0, false, WD_VIDEO_RECOVERY_NONE), "video entry may resume after recovery and cooldown");
}

void test_video_health_distinguishes_audio_wait_from_failure() {
    wd_client_video_health_metrics metrics{};
    metrics.server_frames_tx        = 60;
    metrics.client_reports          = 1;
    metrics.client_frames_seen      = 60;
    metrics.client_frames_decoded   = 60;
    metrics.client_audio_video_sync_holds = 2;
    metrics.client_audio_playback_state = WD_CLIENT_AUDIO_PLAYBACK_BUFFERING;
    metrics.client_audio_video_startup_hold_ms = 500;
    metrics.client_audio_video_sync_hold_current_ms = 500;
    metrics.client_queue_depth      = 3;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_AUDIO_WAIT,
            "decoded frames queued behind audio should not be a video failure");

    metrics.client_audio_video_startup_timeouts = 1;
    metrics.client_audio_video_sync_hold_current_ms = 0;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL,
            "audio startup timeout should expose a presentation stall after the active hold is released");
    metrics.client_audio_video_startup_timeouts = 0;

    metrics.client_audio_video_sync_holds = 0;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_PIPELINE_STALL,
            "decoded frames with no presentation explanation should be a stall");

    metrics.client_decode_failures = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_HARD_FAILURE,
            "explicit decoder errors should dominate health classification");

    metrics.client_decode_failures  = 0;
    metrics.client_frames_presented = 1;
    require(wd_client_video_health_classify(&metrics) == WD_CLIENT_VIDEO_HEALTH_NORMAL,
            "a presented frame should establish normal video health");
}

void test_periodic_capture_is_capped_to_output_refresh() {
    require(wd_cap_periodic_capture_fps(120, 60) == 60, "periodic capture should not outrun the compositor refresh");
    require(wd_cap_periodic_capture_fps(30, 60) == 30, "lower requested capture rates should remain unchanged");
    require(wd_cap_periodic_capture_fps(120, 0) == 120, "unknown refresh rate should not impose a cap");
}

} // namespace

int main() {
    test_wire_bytes_account_for_one_extended_header();
    test_auto_video_entry_uses_sustained_wire_cost();
    test_tile_demand_estimate_uses_all_frame_dirty_average();
    test_video_control_selection_is_authoritative();
    test_compression_requires_material_savings();
    test_locality_with_starvation_bound();
    test_xrgb_compression_prefilter();
    test_compression_benchmark_modes();
    test_compression_advisor_backs_off_and_resamples();
    test_delivery_status_waits_for_seal_and_reports_failure();
    test_periodic_capture_is_capped_to_output_refresh();
    test_video_health_distinguishes_audio_wait_from_failure();
    test_tile_recovery_waits_for_client_presentation();
    return 0;
}
