#pragma once

#include "waydisplay/wd_time.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuration ownership
 * -----------------------
 * This header is the single build-time policy surface for both endpoints.
 * Only launch-, connection-, and hardware-specific choices remain available
 * on the command line; see docs/command-line.md for that contract.
 *
 * Knobs use explicit unit suffixes whenever practical: _NS, _US, _MS,
 * _SECONDS, _BYTES, _KIB, _ENTRIES, _PACKETS, _SAMPLES, _HZ, _PERCENT, and
 * _PX.  Values without a unit are ratios, counts, enum-compatible modes, or
 * derived constants.  Change policy here, rebuild, and run the tests rather
 * than adding environment-variable or legacy command-line aliases.
 */

/* Server startup defaults.
 * CLI-overridable: application, listen address, output scale/refresh,
 * renderer, and video encoder backend.  Configuration-only: Xwayland and
 * xdg-dialog feature policy.  Scale is stored in thousandths so bounds remain
 * exact in compile-time assertions. */
#define WD_SERVER_DEFAULT_APP_COMMAND           "foot"
#define WD_SERVER_DEFAULT_LISTEN_IPV4           "0.0.0.0"
#define WD_SERVER_DEFAULT_OUTPUT_SCALE_MILLI    1000u
#define WD_SERVER_MIN_OUTPUT_SCALE_MILLI        250u
#define WD_SERVER_MAX_OUTPUT_SCALE_MILLI        8000u
#define WD_SERVER_DEFAULT_OUTPUT_SCALE          (WD_SERVER_DEFAULT_OUTPUT_SCALE_MILLI / 1000.0)
#define WD_SERVER_MIN_OUTPUT_SCALE              (WD_SERVER_MIN_OUTPUT_SCALE_MILLI / 1000.0)
#define WD_SERVER_MAX_OUTPUT_SCALE              (WD_SERVER_MAX_OUTPUT_SCALE_MILLI / 1000.0)
#define WD_SERVER_DEFAULT_REFRESH_HZ            60u
#define WD_SERVER_MIN_REFRESH_HZ                1u
#define WD_SERVER_MAX_REFRESH_HZ                1000u
#define WD_SERVER_DEFAULT_RENDERER              "auto"
#define WD_SERVER_DEFAULT_VIDEO_ENCODER_BACKEND "auto"
#define WD_SERVER_DEFAULT_ENABLE_XWAYLAND       1
#define WD_SERVER_DEFAULT_ENABLE_XDG_DIALOG     1

/* Virtual output and tile geometry.
 * WD_DISPLAY_* is the default server output; WD_MAX_RENDER_* is the accepted
 * client/server envelope.  WD_TILE_* is the configuration-only base transport
 * grid.  Compression benchmark mode values are 0=auto, 1=off, 2=attempt, and
 * 3=force; production builds should normally use auto. */
#define WD_DISPLAY_WIDTH  800u
#define WD_DISPLAY_HEIGHT 600u

/* Protocol zero uses 16-bit base-tile IDs and counts. Keep the advertised
 * render surface within a documented 4K-class envelope that is safely
 * representable by the fixed 16x16 base grid. */
#define WD_MAX_RENDER_WIDTH  4096u
#define WD_MAX_RENDER_HEIGHT 2160u

#define WD_TILE_WIDTH                                     16u
#define WD_TILE_HEIGHT                                    16u
#define WD_SERVER_TILE_COMPRESSION_BENCHMARK_MODE_DEFAULT 0u
#define WD_ZSTD_LEVEL                                     1 /* Zstd level for tile payloads; higher uses more CPU. */

#define WD_BASE_TILE_WIDTH  16u
#define WD_BASE_TILE_HEIGHT 16u

#define WD_TILES_X     (((WD_DISPLAY_WIDTH) + (WD_TILE_WIDTH) - 1u) / (WD_TILE_WIDTH))
#define WD_TILES_Y     (((WD_DISPLAY_HEIGHT) + (WD_TILE_HEIGHT) - 1u) / (WD_TILE_HEIGHT))
#define WD_TOTAL_TILES (WD_TILES_X * WD_TILES_Y)

#define WD_BYTES_PER_PIXEL 4u

#define WD_FRAMEBUFFER_PIXELS ((uint32_t)(WD_DISPLAY_WIDTH * WD_DISPLAY_HEIGHT))
#define WD_FRAMEBUFFER_BYTES  ((uint32_t)(WD_FRAMEBUFFER_PIXELS * WD_BYTES_PER_PIXEL))

#define WD_UNCOMPRESSED_TILE_BYTES ((uint32_t)(WD_TILE_WIDTH * WD_TILE_HEIGHT * WD_BYTES_PER_PIXEL))

/* Core network defaults and hard limits.
 * The TCP port is CLI-overridable.  Timeouts bound handshake/send operations;
 * payload and socket-buffer sizes cap memory and datagram behavior.  Probe
 * target/duration control how much traffic is spent estimating link capacity. */
#define WD_DEFAULT_TCP_PORT              5000u
#define WD_TCP_HANDSHAKE_TIMEOUT_MS      3000L
#define WD_TCP_CONNECTED_SEND_TIMEOUT_MS 3000L
#define WD_TCP_FRAME_IDLE_TIMEOUT_NS      (3000ull * WD_NSEC_PER_MSEC)
#define WD_TCP_FRAME_MAX_LIFETIME_NS      (30000ull * WD_NSEC_PER_MSEC)
#define WD_TCP_MAX_PAYLOAD_SIZE          (2u * 1024u * 1024u)
#define WD_UDP_PAYLOAD_TARGET            1400u
#define WD_UDP_SOCKET_BUFFER_BYTES       (16 * 1024 * 1024)
#define WD_MIN_PROBED_UDP_PAYLOAD        512u
#define WD_THROUGHPUT_PROBE_TARGET_BYTES (120u * 1024u * 1024u)
#define WD_THROUGHPUT_PROBE_DURATION_MS  1250u

/* Async I/O ownership, memory, and queue sizing.
 * Ring-entry counts control concurrency.  Pending-byte/packet limits bound
 * retained memory.  Drain limits and sleeps bound shutdown latency.  Raising
 * these values may improve burst tolerance at the cost of memory and longer
 * teardown; lowering them increases backpressure and overflow risk. */
#define WD_ASYNC_MIN_RING_ENTRIES                 8u
#define WD_ASYNC_SENDER_DRAIN_LIMIT               250u
#define WD_ASYNC_SENDER_DRAIN_SLEEP_US            2000u
#define WD_SERVER_ASYNC_TCP_DEFAULT_PENDING_BYTES (4ull * 1024ull * 1024ull)
#define WD_SERVER_ASYNC_UDP_PENDING_MULTIPLIER    4u
#define WD_SERVER_ASYNC_UDP_PENDING_MIN_PACKETS   256u
#define WD_SERVER_ASYNC_UDP_PENDING_MAX_PACKETS   4096u
#define WD_CLIENT_ASYNC_TCP_DEFAULT_PENDING_BYTES (4ull * 1024ull * 1024ull)
#define WD_CLIENT_ASYNC_UDP_DEFAULT_RING_ENTRIES  512u
#define WD_CLIENT_ASYNC_UDP_DEFAULT_PACKET_BYTES  65536u
#define WD_CLIENT_ASYNC_UDP_DEFAULT_DRAIN_BATCH   4096u
#define WD_CLIENT_ASYNC_UDP_COMPLETION_RESERVE    512u

/* Per-channel rings and memory ceilings.  Control favors message count,
 * video favors retained bytes, and UDP favors packet concurrency. */
#define WD_SERVER_CONTROL_TX_RING_ENTRIES         128u
#define WD_SERVER_CONTROL_TX_PENDING_BYTES        (4ull * 1024ull * 1024ull)
#define WD_SERVER_VIDEO_TX_RING_ENTRIES           128u
#define WD_SERVER_VIDEO_TX_PENDING_BYTES          (8ull * 1024ull * 1024ull)
#define WD_SERVER_UDP_TX_RING_ENTRIES             4096u
#define WD_CLIENT_TCP_TX_RING_ENTRIES             128u
#define WD_CLIENT_TCP_TX_PENDING_BYTES            (1024ull * 1024ull)
#define WD_CLIENT_UDP_RX_RING_ENTRIES             512u
#define WD_CLIENT_UDP_RECV_SLACK_BYTES            512u
#define WD_CLIENT_INPUT_TIMESTAMP_HISTORY_ENTRIES 256u
#define WD_NET_LISTEN_BACKLOG                     4

/* Session establishment, probe, gap, and repair deadlines. */
#define WD_NET_AUX_CHANNEL_ACCEPT_TIMEOUT_NS  (500ull * WD_NSEC_PER_MSEC)
#define WD_NET_MTU_PROBE_CLIENT_DEADLINE_NS   (500ull * WD_NSEC_PER_MSEC)
#define WD_NET_THROUGHPUT_DEADLINE_PADDING_MS 500u
#define WD_NET_PROBE_STARTUP_DELAY_MS         10u
#define WD_NET_PROBE_RETRY_SLEEP_MS           1u
#define WD_NET_LINK_PROBE_COUNT               8u
#define WD_NET_CLIENT_GAP_GRACE_NS            (50ull * WD_NSEC_PER_MSEC)
#define WD_NET_CLIENT_REPAIR_RETRY_MIN_NS     (10ull * WD_NSEC_PER_MSEC)

/* IPv4 path-MTU discovery policy.
 * *_BYTES values are link MTU candidates; *_PAYLOAD_* values are conservative
 * application payloads after transport overhead.  The list is ordered from
 * optimistic to conservative and is used automatically, not exposed by CLI. */
#define WD_NET_MTU_PROBE_JUMBO_BYTES    9000u
#define WD_NET_MTU_PROBE_LARGE_BYTES    8192u
#define WD_NET_MTU_PROBE_MEDIUM_BYTES   4096u
#define WD_NET_MTU_PROBE_ETHERNET_BYTES 1500u
#define WD_NET_MTU_PROBE_PPPOE_BYTES    1492u
#define WD_NET_MTU_PROBE_TUNNEL_BYTES   1460u
#define WD_NET_MTU_PROBE_PAYLOAD_VHIGH  1450u
#define WD_NET_MTU_PROBE_PAYLOAD_MHIGH  1440u
#define WD_NET_MTU_PROBE_PAYLOAD_HIGH   1400u
#define WD_NET_MTU_PROBE_PAYLOAD_MEDIUM 1360u
#define WD_NET_MTU_PROBE_PAYLOAD_LOW    1300u
#define WD_NET_MTU_PROBE_PAYLOAD_FLOOR  1200u

/* Logging, telemetry, and feedback cadence.
 * Sample intervals update internal rates; log intervals control operator
 * output; client feedback and summary promotion determine how quickly the
 * server sees runtime pressure.  Shorter intervals react faster but increase
 * wakeups and control traffic. */
#define WD_STATS_SAMPLE_INTERVAL_NS           WD_NSEC_PER_SEC
#define WD_STATS_LOG_INTERVAL_NS              (60ull * WD_STATS_SAMPLE_INTERVAL_NS)
#define WD_LOG_RATE_LIMIT_INTERVAL_NS         WD_STATS_SAMPLE_INTERVAL_NS
#define WD_SERVER_STATS_HEALTH_INTERVAL_NS    WD_STATS_SAMPLE_INTERVAL_NS
#define WD_CLIENT_STATS_FEEDBACK_INTERVAL_NS  WD_STATS_SAMPLE_INTERVAL_NS
#define WD_CLIENT_SUMMARY_PROMOTE_INTERVAL_NS (10ull * 1000ull * 1000ull)

/* Tile-generation summary cadence.
 * Full summaries are large and therefore rare; delta summaries carry normal
 * changes.  The clean interval is slower than the active interval to reduce
 * idle control traffic. */
#define WD_GENERATION_SUMMARY_FULL_SANITY_INTERVAL_NS 60000000000ull
#define WD_GENERATION_SUMMARY_DELTA_INTERVAL_NS       50000000ull
#define WD_GENERATION_SUMMARY_CLEAN_DELTA_INTERVAL_NS 200000000ull

/* Adaptive link-profile bounds and repair pressure.
 * RTT, summary, retransmit, and reassembly values are clamped between MIN and
 * MAX with DEFAULT used before measurement.  Pressure percentages decide when
 * gaps/loss justify repair work.  Larger timing values tolerate jitter but
 * delay recovery; smaller values recover faster but can cause duplicate work. */
#define WD_LINK_RTT_MIN_NS     3000000ull
#define WD_LINK_RTT_DEFAULT_NS 100000000ull
#define WD_LINK_RTT_MAX_NS     800000000ull

#define WD_LINK_SUMMARY_GRACE_MIN_NS     150000000ull
#define WD_LINK_SUMMARY_GRACE_DEFAULT_NS 150000000ull
#define WD_LINK_SUMMARY_GRACE_MAX_NS     1200000000ull

#define WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MIN_NS     250000000ull
#define WD_LINK_RETRANSMIT_REQUEST_INTERVAL_DEFAULT_NS 250000000ull
#define WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MAX_NS     1200000000ull

#define WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS     250000000ull
#define WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS 250000000ull
#define WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS     1200000000ull

#define WD_LINK_TILE_REASSEMBLY_MIN_NS     150000000ull
#define WD_LINK_TILE_REASSEMBLY_DEFAULT_NS 250000000ull
#define WD_LINK_TILE_REASSEMBLY_MAX_NS     900000000ull

#define WD_LINK_ACTIVE_SUMMARY_INTERVAL_MIN_NS     50000000ull
#define WD_LINK_ACTIVE_SUMMARY_INTERVAL_DEFAULT_NS WD_GENERATION_SUMMARY_DELTA_INTERVAL_NS
#define WD_LINK_ACTIVE_SUMMARY_INTERVAL_MAX_NS     500000000ull

#define WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS     200000000ull
#define WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS WD_GENERATION_SUMMARY_CLEAN_DELTA_INTERVAL_NS
#define WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS     WD_NSEC_PER_SEC

#define WD_LINK_SUMMARY_BUDGET_PERCENT              5u
#define WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS         50000000ull
#define WD_LINK_RUNTIME_GAP_PRESSURE_DECAY_PERCENT  75u
#define WD_LINK_STALE_REPAIR_BACKOFF_PERCENT        75u
#define WD_LINK_STALE_REPAIR_BACKOFF_MULTIPLIER     2u
#define WD_LINK_LARGE_SUMMARY_REPAIR_PERCENT        25u
#define WD_LINK_LARGE_SUMMARY_REPAIR_LOSS_SIGNAL_NS 2000000000ull
#define WD_LINK_LARGE_SUMMARY_REPAIR_COOLDOWN_NS    WD_NSEC_PER_SEC
#define WD_CLIENT_REPAIR_PRESSURE_PERCENT           25u
#define WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES   2048u

/* Audio and automatic-video product policy.
 * Audio values define the negotiated product defaults.  Video dirty coverage
 * and enter/exit durations govern automatic switching between tile and video
 * transport.  These detailed thresholds are intentionally configuration-only. */
#define WD_AUDIO_SAMPLE_RATE_DEFAULT        48000u
#define WD_AUDIO_FRAME_SAMPLES_DEFAULT      960u
#define WD_AUDIO_TARGET_LATENCY_MS_DEFAULT  20u
#define WD_AUDIO_TARGET_LATENCY_MS_MIN      10u
#define WD_AUDIO_TARGET_LATENCY_MS_MAX      400u
#define WD_AUDIO_BITRATE_DEFAULT            128000u
#define WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT  60u
#define WD_VIDEO_MIN_DIRTY_PERCENT_MAX      100u
#define WD_VIDEO_ENTER_SECONDS_DEFAULT      3u
#define WD_VIDEO_ENTER_SECONDS_MAX          60u
#define WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT 30u
#define WD_VIDEO_EXIT_DIRTY_PERCENT_MAX     100u
#define WD_VIDEO_EXIT_SECONDS_DEFAULT       30u
#define WD_VIDEO_EXIT_SECONDS_MAX           300u

/* Audio/video pipeline buffering and scheduling.
 * Audio ring/queue values trade latency for underrun tolerance.  Worker/drain
 * timings control wakeups and bounded shutdown.  Client early/late thresholds
 * decide whether video waits for audio or is dropped to catch up. */
#define WD_AUDIO_CAPTURE_RING_MS                    200u
#define WD_AUDIO_TX_QUEUE_MS                        100u
#define WD_AUDIO_TX_MIN_PENDING_BYTES               4096u
#define WD_AUDIO_WORKER_SLEEP_US                    2000u
#define WD_AUDIO_CAPTURE_TIMESTAMP_TOLERANCE_CYCLES 2u
#define WD_AUDIO_CAPTURE_IDLE_DIAGNOSTIC_NS         (10ull * WD_NSEC_PER_SEC)
#define WD_AUDIO_TX_RING_ENTRIES                    64u
#define WD_AUDIO_TX_DRAIN_POLLS                     20u
#define WD_AUDIO_TX_DRAIN_SLEEP_US                  1000u
#define WD_CLIENT_VIDEO_AUDIO_EARLY_MS              40u
#define WD_CLIENT_VIDEO_AUDIO_LATE_MS               80u
#define WD_CLIENT_VIDEO_AUDIO_MAX_RETRY_MS          20u
#define WD_CLIENT_AUDIO_LATE_PACKET_MS              120u

/* Clipboard and primary-selection capture.
 * INITIAL_BYTES is the first allocation for asynchronous reads; TIMEOUT_MS
 * prevents a selection owner from holding compositor resources indefinitely.
 * Protocol payload limits remain in wd_protocol.h. */
#define WD_SELECTION_CAPTURE_INITIAL_BYTES 65536u
#define WD_SELECTION_CAPTURE_TIMEOUT_MS    2000u

/* Encoder implementation policy.
 * Software threads bound CPU parallelism.  GOP controls keyframe cadence.
 * VAAPI probe settings test capability cheaply; pool/async depth control the
 * number of hardware surfaces and queued operations. */
#define WD_VIDEO_ENCODER_FALLBACK_FPS            30u
#define WD_VIDEO_ENCODER_SOFTWARE_THREADS        2u
#define WD_VIDEO_ENCODER_GOP_SECONDS             1u
#define WD_VIDEO_ENCODER_VAAPI_PROBE_FPS         30u
#define WD_VIDEO_ENCODER_VAAPI_PROBE_BITRATE_KIB 2048u
#define WD_VIDEO_ENCODER_VAAPI_PROBE_POOL_SIZE   2u
#define WD_VIDEO_ENCODER_VAAPI_FRAME_POOL_SIZE   4u
#define WD_VIDEO_ENCODER_VAAPI_ASYNC_DEPTH       "1"

/* Client request defaults and top-level stream budgets.
 * FPS and adaptive UDP rate bounds constrain client requests.  Throughput safety
 * reserves link headroom.  Video bitrate 0 in a request means derive from the
 * link; WD_VIDEO_DEFAULT_* is the fallback when a fixed estimate is required.
 * Hardware-decode values are enum-compatible client policy modes. */
#define WD_DEFAULT_CAPTURE_FPS                       60u
#define WD_MAX_REASONABLE_FPS                        240u
#define WD_STREAM_TOKEN_BURST_DIVISOR                4u
#define WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND  (1024ull * 1024ull)
#define WD_UDP_RATE_MIN_BYTES_PER_SECOND      (25ull * 1024ull)
#define WD_UDP_RATE_MAX_BYTES_PER_SECOND      (1000ull * 1024ull * 1024ull * 1024ull)
#define WD_UDP_THROUGHPUT_SAFETY_PERCENT      85u
#define WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND      8192u
#define WD_VIDEO_DERIVED_BITRATE_MAX_KIB_PER_SECOND  50000u
#define WD_CLIENT_VIDEO_HWDECODE_AUTO                0u
#define WD_CLIENT_VIDEO_HWDECODE_OFF                 1u
#define WD_CLIENT_VIDEO_HWDECODE_VAAPI               2u

/* Stream adaptation and link-health policy.
 * Loss/pressure streaks reduce rate or FPS; sustained good periods increase
 * them.  Recovery/cooldown values prevent oscillation.  Wire tile size is
 * chosen per dirty region from the supported ladder rather than by a CLI mode. */
#define WD_WIRE_TILE_MAX_WIDTH                               128u
#define WD_WIRE_TILE_MAX_HEIGHT                              64u
#define WD_STREAM_LINK_LOSS_SECONDS_TO_DECREASE              1u
#define WD_STREAM_LINK_GOOD_SECONDS_TO_INCREASE              4u
#define WD_STREAM_RATE_PRESSURE_DECREASE_PERCENT             65u
#define WD_STREAM_RATE_INCREASE_PERCENT                      125u
#define WD_STREAM_RATE_INCREASE_MIN_BYTES                    (64ull * 1024ull)
#define WD_STREAM_CLIENT_COMPLETION_MIN_PACKETS              4u
#define WD_STREAM_CLIENT_COMPLETION_LOSS_PERCENT             35u
#define WD_STREAM_CLIENT_RENDER_FPS_PRESSURE_PERCENT         70u
#define WD_STREAM_CLIENT_RENDER_PRESSURE_SECONDS_TO_DECREASE 3u
#define WD_STREAM_MULTIPACKET_LOSS_COOLDOWN_SECONDS          2u
#define WD_STREAM_TILE_RECOVERY_TIMEOUT_SECONDS              5u
#define WD_STREAM_VIDEO_RETRY_COOLDOWN_SECONDS               5u
#define WD_STREAM_BOOTSTRAP_SUPPRESSION_TIMEOUT_SECONDS      5u

#define WD_STREAM_FPS_MIN                       5u
#define WD_STREAM_HIDDEN_CLIENT_FPS             5u
#define WD_STREAM_FPS_DECREASE_PERCENT          65u
#define WD_STREAM_FPS_PRESSURE_DECREASE_PERCENT 70u
#define WD_STREAM_FPS_INCREASE_PERCENT          140u
#define WD_STREAM_FPS_GOOD_SECONDS_TO_INCREASE  3u

/* Stream workers, compression, and automatic video entry.
 * Thread/result limits bound CPU and queue fan-out.  Compression requires both
 * absolute and percentage savings.  The tile advisor samples entropy and
 * bypasses repeatedly poor candidates.  Automatic video entry combines dirty
 * coverage, sustained change, wire pressure, and FPS pressure. */
#define WD_STREAM_ENCODER_MAX_THREADS                  4u
#define WD_STREAM_ENCODER_RESERVED_CPUS                1u
#define WD_STREAM_ENCODER_MAX_RESULTS_PER_JOB          32u
#define WD_STREAM_DIRTY_REGION_STARVATION_NS           (100ull * WD_NSEC_PER_MSEC)
#define WD_STREAM_TILE_COMPRESSION_MIN_SAVINGS_BYTES   64u
#define WD_STREAM_TILE_COMPRESSION_MIN_SAVINGS_PERCENT 3u
#define WD_STREAM_VIDEO_CLIENT_FAILURE_SECONDS         3u
#define WD_STREAM_VIDEO_DERIVED_BUDGET_PERCENT         75u

/* Supported wire-tile ladder, largest to base.  Keep endpoints aligned with
 * the protocol geometry and derive counts instead of repeating literals. */
#define WD_TILE_SIZE_MEGA_WIDTH      256u
#define WD_TILE_SIZE_MEGA_HEIGHT     256u
#define WD_TILE_SIZE_HUGE_WIDTH      128u
#define WD_TILE_SIZE_HUGE_HEIGHT     128u
#define WD_TILE_SIZE_LARGE_WIDTH     128u
#define WD_TILE_SIZE_LARGE_HEIGHT    64u
#define WD_TILE_SIZE_MEDIUM_WIDTH    64u
#define WD_TILE_SIZE_MEDIUM_HEIGHT   64u
#define WD_TILE_SIZE_SMALL_WIDTH     32u
#define WD_TILE_SIZE_SMALL_HEIGHT    32u
#define WD_TILE_SIZE_BASE_WIDTH      16u
#define WD_TILE_SIZE_BASE_HEIGHT     16u
#define WD_SUPPORTED_TILE_SIZE_COUNT 4u
#define WD_WIRE_TILE_MAX_BASE_TILES  ((WD_WIRE_TILE_MAX_WIDTH / WD_BASE_TILE_WIDTH) * (WD_WIRE_TILE_MAX_HEIGHT / WD_BASE_TILE_HEIGHT))

/* Tile compressibility advisor and automatic-video thresholds. */
#define WD_TILE_ADVISOR_ENTROPY_SAMPLE_COUNT         64u
#define WD_TILE_ADVISOR_SMALL_PAYLOAD_PIXELS         16u
#define WD_TILE_ADVISOR_MAX_UNIQUE_NUMERATOR         7u
#define WD_TILE_ADVISOR_MAX_UNIQUE_DENOMINATOR       8u
#define WD_TILE_ADVISOR_MIN_ADJACENT_REPEATS         2u
#define WD_TILE_ADVISOR_REPEATED_DELTA_DIVISOR       8u
#define WD_TILE_ADVISOR_BYPASS_PROBE_INTERVAL        16u
#define WD_TILE_ADVISOR_POOR_STREAK_LIMIT            8u
#define WD_TILE_ADVISOR_BYPASS_ATTEMPTS              64u
#define WD_TILE_AUTO_ENTRY_MIN_DIRTY_PERCENT_DEFAULT WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT
#define WD_TILE_AUTO_ENTRY_DIRTY_FLOOR_DIVISOR       3u
#define WD_TILE_AUTO_ENTRY_DIRTY_FLOOR_MIN_PERCENT   15u
#define WD_TILE_AUTO_ENTRY_PEAK_FLOOR_MIN_PERCENT    50u
#define WD_TILE_AUTO_ENTRY_CHANGED_FRAMES_PERCENT    25u
#define WD_TILE_AUTO_ENTRY_WIRE_PRESSURE_PERCENT     65u
#define WD_TILE_AUTO_ENTRY_FPS_PRESSURE_PERCENT      85u

/* Link-timer formula coefficients.
 * These multipliers, divisors, and fixed margins turn measured RTT/jitter into
 * summary, retransmit, and reassembly deadlines before clamping to the bounds
 * above. */
#define WD_LINK_PROFILE_JITTER_MULTIPLIER          2u
#define WD_LINK_PROFILE_SUMMARY_MARGIN_NS          (50ull * WD_NSEC_PER_MSEC)
#define WD_LINK_PROFILE_RETRANSMIT_MARGIN_NS       (100ull * WD_NSEC_PER_MSEC)
#define WD_LINK_PROFILE_REASSEMBLY_RTT_DIVISOR     2u
#define WD_LINK_PROFILE_REASSEMBLY_MARGIN_NS       (150ull * WD_NSEC_PER_MSEC)
#define WD_LINK_PROFILE_ACTIVE_SUMMARY_RTT_DIVISOR 4u
#define WD_LINK_PROFILE_CLEAN_SUMMARY_RTT_DIVISOR  2u
#define WD_LINK_PROFILE_CLEAN_TO_ACTIVE_MULTIPLIER 2u

/* Client window, render loop, decoder queues, and UI behavior.
 * Debounce/wait values trade responsiveness for wakeups.  Upload thresholds
 * choose dirty rectangles versus bounding/full uploads.  Decoder queue
 * capacities are ordered metadata >= decoded >= present.  Context-menu and
 * wheel values are local UI geometry, not protocol policy. */
#define WD_CLIENT_DEFAULT_TARGET_FPS               WD_DEFAULT_CAPTURE_FPS
#define WD_CLIENT_RESIZE_DEBOUNCE_NS               150000000ull
#define WD_CLIENT_FRAME_DELAY_MS                   8
#define WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_THRESHOLD 256u
#define WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_PERCENT   60u
/* Render-planning calibration.  Fixed costs are elapsed nanoseconds rather
 * than equivalent pixel counts; the implementation combines them with the
 * measured per-pixel cost. */
/* Render-cost calibration.  Fixed call costs are nanoseconds; per-pixel cost
 * uses Q16 fixed point and an EWMA with clamped samples. */
#define WD_CLIENT_RENDER_COST_MIN_SAMPLES           4u
#define WD_CLIENT_TEXTURE_UPDATE_CALL_COST_NS       16384ull
#define WD_CLIENT_TEXTURE_LOCK_CALL_COST_NS         131072ull
#define WD_CLIENT_RENDER_COST_EWMA_OLD_NUMERATOR    7u
#define WD_CLIENT_RENDER_COST_EWMA_DENOMINATOR      8u
#define WD_CLIENT_RENDER_COST_SAMPLE_CLAMP_DIVISOR  4u
#define WD_CLIENT_RENDER_COST_MIN_PIXEL_SAMPLE      (64ull * 1024ull)
#define WD_CLIENT_RENDER_COST_MIN_Q16               (1ull << 8u)
#define WD_CLIENT_RENDER_COST_MAX_Q16               (1000ull << 16u)
#define WD_CLIENT_BOUNDS_UPLOAD_MIN_SAVINGS_PERCENT 15u
#define WD_CLIENT_FULL_UPLOAD_MIN_SAVINGS_PERCENT   20u
/* Client window, framebuffer, decoder, and local UI bounds. */
#define WD_CLIENT_MIN_WINDOW_WIDTH              256
#define WD_CLIENT_MIN_WINDOW_HEIGHT             256
#define WD_CLIENT_MAX_FRAMEBUFFER_BYTES         (512ull * 1024ull * 1024ull)
#define WD_CLIENT_VIDEO_METADATA_QUEUE_CAPACITY 64u
#define WD_CLIENT_VIDEO_DECODED_QUEUE_CAPACITY  8u
#define WD_CLIENT_VIDEO_PRESENT_QUEUE_CAPACITY  6u
#define WD_CLIENT_WHEEL_AXIS_STEP               60
#define WD_CLIENT_CONTEXT_MENU_PADDING_X        8
#define WD_CLIENT_CONTEXT_MENU_PADDING_Y        7
#define WD_CLIENT_CONTEXT_MENU_ITEM_HEIGHT      20
#define WD_CLIENT_CONTEXT_MENU_WIDTH            184
#define WD_CLIENT_CONTEXT_MENU_TEXT_SCALE       1
#define WD_CLIENT_CONTEXT_MENU_TEXT_X           8
#define WD_CLIENT_CONTEXT_MENU_TEXT_Y           6
/* Client event-loop networking and runtime feedback cadence. */
#define WD_CLIENT_UDP_DRAIN_BATCH                     256u
#define WD_CLIENT_TCP_DRAIN_BATCH                     16u
#define WD_CLIENT_MAX_IDLE_WAIT_NS                    (50ull * WD_NSEC_PER_MSEC)
#define WD_CLIENT_CONFIG_SYNC_WAIT_NS                 (10ull * WD_NSEC_PER_MSEC)
#define WD_CLIENT_RUNTIME_GAP_MIN_SAMPLES             16u
#define WD_CLIENT_FRAMEBUFFER_LOCK_EWMA_OLD_NUMERATOR 7u
#define WD_CLIENT_FRAMEBUFFER_LOCK_EWMA_DENOMINATOR   8u

/* Server compositor, input, and child-process behavior.
 * Move/resize zones and minimum sizes shape interaction.  Frame-service and
 * repair sampling control compositor work cadence.  Input capacities bound
 * queued events.  Repeat settings emulate a keyboard.  Process grace periods
 * bound TERM/KILL shutdown and polling. */
#define WD_FALLBACK_MOVE_ZONE_HEIGHT        32.0
#define WD_RESIZE_EDGE_ZONE                 8.0
#define WD_MIN_WINDOW_WIDTH                 120u
#define WD_MIN_WINDOW_HEIGHT                80u
#define WD_XDG_ACTIVATION_TOKEN_TIMEOUT_MS  10000
#define WD_SERVER_FRAME_SERVICE_MIN_INTERVAL_MS 1u
#define WD_SERVER_FRAME_SERVICE_MAX_INTERVAL_MS 8u
#define WD_SERVER_STALE_REPAIR_MIN_SAMPLES  16u
#define WD_SERVER_KEY_QUEUE_CAPACITY        4096u
#define WD_SERVER_POINTER_QUEUE_CAPACITY    4096u
#define WD_SERVER_PRESSED_KEY_CAPACITY      256u
#define WD_SERVER_KEYBOARD_REPEAT_RATE_HZ   25u
#define WD_SERVER_KEYBOARD_REPEAT_DELAY_MS  600u
#define WD_SERVER_PROCESS_TERM_GRACE_MS     1000u
#define WD_SERVER_PROCESS_KILL_GRACE_MS     1000u
#define WD_SERVER_PROCESS_POLL_INTERVAL_MS  10u

/* Initial native-surface scene placement.
 * Margins/insets keep new windows visible, cascade values spread peers, and
 * fallback dimensions place dialogs/children that do not provide usable
 * geometry. */
#define WD_SCENE_DIALOG_PARENT_THRESHOLD_PX      160u
#define WD_SCENE_DIALOG_PARENT_INSET_PX          80u
#define WD_SCENE_OUTPUT_MARGIN_X_PX              160u
#define WD_SCENE_OUTPUT_MARGIN_Y_PX              120u
#define WD_SCENE_DIALOG_MIN_WIDTH_PX             320u
#define WD_SCENE_DIALOG_MIN_HEIGHT_PX            200u
#define WD_SCENE_CASCADE_POSITION_COUNT          8u
#define WD_SCENE_CASCADE_STEP_PX                 40u
#define WD_SCENE_CHILD_FALLBACK_WIDTH_PX         480u
#define WD_SCENE_CHILD_FALLBACK_HEIGHT_PX        320u
#define WD_SCENE_CHILD_VERTICAL_POSITION_DIVISOR 3u

/* Xwayland fallback geometry and server-side decorations.
 * Used only when WD_SERVER_DEFAULT_ENABLE_XWAYLAND is enabled and the build
 * contains Xwayland support.  Pixel values define initial visibility and
 * titlebar/button layout. */
#define WD_XWAYLAND_DEFAULT_WIDTH      800u
#define WD_XWAYLAND_DEFAULT_HEIGHT     600u
#define WD_XWAYLAND_MIN_VISIBLE_WIDTH  64u
#define WD_XWAYLAND_MIN_VISIBLE_HEIGHT 48u
#define WD_XWAYLAND_TITLEBAR_HEIGHT    28u
#define WD_XWAYLAND_BUTTON_SIZE        18u
#define WD_XWAYLAND_BUTTON_MARGIN      5u
#define WD_XWAYLAND_BUTTON_GAP         5u

/* Configuration invariants.  Keep these close to the defaults so invalid
 * combinations fail every C and C++ build instead of surfacing at runtime. */
#if defined(__cplusplus)
#define WD_CONFIG_STATIC_ASSERT(condition, message) static_assert((condition), message)
#else
#define WD_CONFIG_STATIC_ASSERT(condition, message) _Static_assert((condition), message)
#endif

WD_CONFIG_STATIC_ASSERT(WD_DEFAULT_TCP_PORT > 0u && WD_DEFAULT_TCP_PORT <= UINT16_MAX, "default TCP port must fit uint16_t");
WD_CONFIG_STATIC_ASSERT(WD_ASYNC_MIN_RING_ENTRIES > 0u && WD_SERVER_CONTROL_TX_RING_ENTRIES >= WD_ASYNC_MIN_RING_ENTRIES &&
                            WD_SERVER_VIDEO_TX_RING_ENTRIES >= WD_ASYNC_MIN_RING_ENTRIES &&
                            WD_CLIENT_TCP_TX_RING_ENTRIES >= WD_ASYNC_MIN_RING_ENTRIES,
                        "async ring defaults must satisfy the implementation minimum");
WD_CONFIG_STATIC_ASSERT(WD_SERVER_ASYNC_UDP_PENDING_MIN_PACKETS <= WD_SERVER_ASYNC_UDP_PENDING_MAX_PACKETS,
                        "server UDP pending bounds must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_NET_MTU_PROBE_PAYLOAD_FLOOR >= WD_MIN_PROBED_UDP_PAYLOAD, "MTU payload floor must satisfy the accepted minimum");
WD_CONFIG_STATIC_ASSERT(WD_SERVER_MIN_OUTPUT_SCALE_MILLI > 0u && WD_SERVER_MIN_OUTPUT_SCALE_MILLI <= WD_SERVER_DEFAULT_OUTPUT_SCALE_MILLI &&
                            WD_SERVER_DEFAULT_OUTPUT_SCALE_MILLI <= WD_SERVER_MAX_OUTPUT_SCALE_MILLI,
                        "server output-scale defaults must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_STREAM_TILE_COMPRESSION_MIN_SAVINGS_PERCENT <= 100u && WD_STREAM_VIDEO_DERIVED_BUDGET_PERCENT <= 100u &&
                            WD_TILE_AUTO_ENTRY_CHANGED_FRAMES_PERCENT <= 100u && WD_TILE_AUTO_ENTRY_WIRE_PRESSURE_PERCENT <= 100u &&
                            WD_TILE_AUTO_ENTRY_FPS_PRESSURE_PERCENT <= 100u,
                        "stream percentages must be valid");
WD_CONFIG_STATIC_ASSERT(WD_WIRE_TILE_MAX_WIDTH % WD_BASE_TILE_WIDTH == 0u && WD_WIRE_TILE_MAX_HEIGHT % WD_BASE_TILE_HEIGHT == 0u &&
                            WD_WIRE_TILE_MAX_BASE_TILES > 0u && WD_WIRE_TILE_MAX_BASE_TILES <= UINT16_MAX,
                        "wire-tile coverage must fit base-tile storage");
WD_CONFIG_STATIC_ASSERT(WD_TILE_SIZE_LARGE_WIDTH == WD_WIRE_TILE_MAX_WIDTH && WD_TILE_SIZE_LARGE_HEIGHT == WD_WIRE_TILE_MAX_HEIGHT &&
                            WD_TILE_SIZE_BASE_WIDTH == WD_BASE_TILE_WIDTH && WD_TILE_SIZE_BASE_HEIGHT == WD_BASE_TILE_HEIGHT,
                        "supported tile endpoints must match protocol geometry");
WD_CONFIG_STATIC_ASSERT(WD_AUDIO_TARGET_LATENCY_MS_MIN <= WD_AUDIO_TARGET_LATENCY_MS_DEFAULT &&
                            WD_AUDIO_TARGET_LATENCY_MS_DEFAULT <= WD_AUDIO_TARGET_LATENCY_MS_MAX,
                        "audio latency defaults must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_AUDIO_SAMPLE_RATE_DEFAULT > 0u && WD_AUDIO_FRAME_SAMPLES_DEFAULT > 0u &&
                            WD_AUDIO_SAMPLE_RATE_DEFAULT % 50u == 0u &&
                            WD_AUDIO_FRAME_SAMPLES_DEFAULT == WD_AUDIO_SAMPLE_RATE_DEFAULT / 50u,
                        "default audio frame must represent 20 milliseconds");
WD_CONFIG_STATIC_ASSERT(WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT <= WD_VIDEO_MIN_DIRTY_PERCENT_MAX &&
                            WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT <= WD_VIDEO_EXIT_DIRTY_PERCENT_MAX &&
                            WD_VIDEO_MIN_DIRTY_PERCENT_MAX <= 100u && WD_VIDEO_EXIT_DIRTY_PERCENT_MAX <= 100u,
                        "video dirty thresholds must be valid percentages");
WD_CONFIG_STATIC_ASSERT(WD_VIDEO_ENTER_SECONDS_DEFAULT <= WD_VIDEO_ENTER_SECONDS_MAX &&
                            WD_VIDEO_EXIT_SECONDS_DEFAULT <= WD_VIDEO_EXIT_SECONDS_MAX,
                        "video mode durations must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_CLIENT_FRAMEBUFFER_LOCK_EWMA_OLD_NUMERATOR < WD_CLIENT_FRAMEBUFFER_LOCK_EWMA_DENOMINATOR,
                        "framebuffer lock EWMA must retain some new samples");
WD_CONFIG_STATIC_ASSERT(WD_SERVER_KEY_QUEUE_CAPACITY > 0u && WD_SERVER_POINTER_QUEUE_CAPACITY > 0u && WD_SERVER_PRESSED_KEY_CAPACITY > 0u,
                        "input queue capacities must be nonzero");
WD_CONFIG_STATIC_ASSERT(WD_SERVER_PROCESS_TERM_GRACE_MS > 0u && WD_SERVER_PROCESS_KILL_GRACE_MS > 0u &&
                            WD_SERVER_PROCESS_POLL_INTERVAL_MS > 0u,
                        "process shutdown timing must be bounded");
WD_CONFIG_STATIC_ASSERT(WD_SERVER_MIN_REFRESH_HZ > 0u && WD_SERVER_MIN_REFRESH_HZ <= WD_SERVER_DEFAULT_REFRESH_HZ &&
                            WD_SERVER_DEFAULT_REFRESH_HZ <= WD_SERVER_MAX_REFRESH_HZ,
                        "server refresh defaults must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_TILE_WIDTH > 0u && WD_TILE_HEIGHT > 0u, "tile dimensions must be nonzero");
WD_CONFIG_STATIC_ASSERT(WD_SERVER_TILE_COMPRESSION_BENCHMARK_MODE_DEFAULT <= 3u, "tile compression benchmark mode must be valid");
WD_CONFIG_STATIC_ASSERT(WD_BASE_TILE_WIDTH > 0u && WD_BASE_TILE_HEIGHT > 0u, "base tile dimensions must be nonzero");
WD_CONFIG_STATIC_ASSERT(WD_WIRE_TILE_MAX_WIDTH % WD_BASE_TILE_WIDTH == 0u && WD_WIRE_TILE_MAX_HEIGHT % WD_BASE_TILE_HEIGHT == 0u,
                        "wire tile dimensions must align to the base grid");
WD_CONFIG_STATIC_ASSERT(WD_DISPLAY_WIDTH <= WD_MAX_RENDER_WIDTH && WD_DISPLAY_HEIGHT <= WD_MAX_RENDER_HEIGHT,
                        "default display must fit the render envelope");
WD_CONFIG_STATIC_ASSERT(WD_CLIENT_DIRTY_RECT_FULL_UPLOAD_PERCENT <= 100u && WD_CLIENT_BOUNDS_UPLOAD_MIN_SAVINGS_PERCENT <= 100u &&
                            WD_CLIENT_FULL_UPLOAD_MIN_SAVINGS_PERCENT <= 100u,
                        "render percentages must be valid");
WD_CONFIG_STATIC_ASSERT(WD_UDP_THROUGHPUT_SAFETY_PERCENT <= 100u && WD_LINK_SUMMARY_BUDGET_PERCENT <= 100u &&
                            WD_CLIENT_REPAIR_PRESSURE_PERCENT <= 100u,
                        "network percentages must be valid");
WD_CONFIG_STATIC_ASSERT(WD_UDP_RATE_MIN_BYTES_PER_SECOND <= WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND &&
                            WD_UDP_RATE_DEFAULT_BYTES_PER_SECOND <= WD_UDP_RATE_MAX_BYTES_PER_SECOND,
                        "UDP rate defaults must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_CLIENT_VIDEO_METADATA_QUEUE_CAPACITY >= WD_CLIENT_VIDEO_DECODED_QUEUE_CAPACITY &&
                            WD_CLIENT_VIDEO_DECODED_QUEUE_CAPACITY >= WD_CLIENT_VIDEO_PRESENT_QUEUE_CAPACITY,
                        "video queue capacities must be ordered");
WD_CONFIG_STATIC_ASSERT(WD_AUDIO_CAPTURE_RING_MS >= WD_AUDIO_TX_QUEUE_MS, "audio capture buffering must cover the transmit queue");
WD_CONFIG_STATIC_ASSERT(WD_CLIENT_VIDEO_AUDIO_EARLY_MS < WD_CLIENT_VIDEO_AUDIO_LATE_MS,
                        "video early hold must be smaller than the late-drop threshold");
WD_CONFIG_STATIC_ASSERT(WD_VIDEO_ENCODER_FALLBACK_FPS > 0u && WD_VIDEO_ENCODER_SOFTWARE_THREADS > 0u && WD_VIDEO_ENCODER_GOP_SECONDS > 0u,
                        "video encoder defaults must be nonzero");
WD_CONFIG_STATIC_ASSERT(WD_CLIENT_RENDER_COST_EWMA_OLD_NUMERATOR < WD_CLIENT_RENDER_COST_EWMA_DENOMINATOR,
                        "render EWMA must retain less than one full sample");

#undef WD_CONFIG_STATIC_ASSERT

#ifdef __cplusplus
}
#endif
