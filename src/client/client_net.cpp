#include "client_net.hpp"

#include "client_transport.hpp"

#include "client_async_tcp.hpp"
#include "client_async_udp.hpp"
#include "client_config_validation.hpp"
#include "client_receive.hpp"
#include "content_order.hpp"
#include "video_decoder.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_media_clock.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_protocol_dispatch.h"
#include "waydisplay/wd_selection.h"
#include "waydisplay/wd_time.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <climits>
#include <unistd.h>
#include <vector>

namespace waydisplay {
namespace {

constexpr size_t MAX_TILE_REPAIR_REQUEST_PAYLOAD_BYTES = WD_TCP_MAX_PAYLOAD_SIZE;
constexpr size_t MAX_RETRANSMIT_REQUEST_ENTRY_CAP =
    (MAX_TILE_REPAIR_REQUEST_PAYLOAD_BYTES - sizeof(wd_tile_repair_request_payload_header)) / sizeof(wd_tile_repair_entry);
constexpr size_t MAX_RETRANSMIT_ENTRIES_PER_MESSAGE =
    MAX_RETRANSMIT_REQUEST_ENTRY_CAP > UINT16_MAX ? UINT16_MAX : MAX_RETRANSMIT_REQUEST_ENTRY_CAP;
constexpr uint64_t RETRANSMIT_GRACE_MIN_NS           = WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS;
constexpr uint64_t RETRANSMIT_GRACE_DEFAULT_NS       = WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS;
constexpr uint64_t RETRANSMIT_GRACE_MAX_NS           = WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS;
constexpr uint64_t MTU_PROBE_SERVER_STARTUP_DELAY_NS = WD_NET_PROBE_STARTUP_DELAY_MS * WD_NSEC_PER_MSEC;
constexpr size_t CLIENT_VIDEO_DECODE_QUEUE_CAPACITY = 4;
constexpr size_t CLIENT_AUDIO_DECODE_QUEUE_CAPACITY = 32;

const char* video_mode_name(uint8_t mode) {
    switch (mode)
    {
    case WD_VIDEO_MODE_AUTO:
        return "auto";
    case WD_VIDEO_MODE_OFF:
        return "off";
    case WD_VIDEO_MODE_FORCE:
        return "force";
    default:
        return "unknown";
    }
}

const char* video_codec_name(uint32_t codec) {
    switch (codec)
    {
    case WD_VIDEO_CODEC_H264:
        return "h264";
    case WD_VIDEO_CODEC_H265:
        return "h265";
    case WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265:
        return "auto";
    default:
        return "none";
    }
}

const char* video_hwdecode_mode_name(uint8_t mode) {
    switch (mode)
    {
    case WD_CLIENT_VIDEO_HWDECODE_AUTO:
        return "auto";
    case WD_CLIENT_VIDEO_HWDECODE_OFF:
        return "off";
    case WD_CLIENT_VIDEO_HWDECODE_VAAPI:
        return "vaapi";
    default:
        return "unknown";
    }
}

uint64_t clamp_retransmit_grace_ns(uint64_t ns) {
    return std::max(RETRANSMIT_GRACE_MIN_NS, std::min(RETRANSMIT_GRACE_MAX_NS, ns));
}

uint64_t ms_to_ns(uint16_t ms, uint64_t fallback_ns) {
    if (ms == 0)
    {
        return fallback_ns;
    }
    return static_cast<uint64_t>(ms) * WD_NSEC_PER_MSEC;
}

uint64_t clamp_timer_ns(uint64_t ns, uint64_t min_ns, uint64_t max_ns) {
    return std::max(min_ns, std::min(max_ns, ns));
}

uint64_t udp_gap_pressure_ns(const ClientState& state) {
    return clamp_timer_ns(state.udp_gap_pressure_ns.load(std::memory_order_relaxed), 0, WD_LINK_RTT_MAX_NS);
}

uint64_t summary_retransmit_grace_ns(const ClientState& state) {
    uint64_t grace_ns = clamp_timer_ns(state.summary_retransmit_grace_ns.load(std::memory_order_relaxed), WD_LINK_SUMMARY_GRACE_MIN_NS,
                                       WD_LINK_SUMMARY_GRACE_MAX_NS);
    uint64_t gap_ns   = udp_gap_pressure_ns(state);
    if (gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        uint64_t gap_grace_ns = gap_ns + WD_NET_CLIENT_GAP_GRACE_NS;
        grace_ns = std::max(grace_ns, clamp_timer_ns(gap_grace_ns, WD_LINK_SUMMARY_GRACE_MIN_NS, WD_LINK_SUMMARY_GRACE_MAX_NS));
    }
    return grace_ns;
}

uint64_t retransmit_request_interval_ns(const ClientState& state) {
    uint64_t request_interval_ns = clamp_timer_ns(state.retransmit_request_interval_ns.load(std::memory_order_relaxed),
                                           WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MIN_NS, WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MAX_NS);
    uint64_t gap_ns       = udp_gap_pressure_ns(state);
    if (gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        request_interval_ns = std::max(request_interval_ns,
                                       clamp_timer_ns(gap_ns * 2ull, WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MIN_NS,
                                                      WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MAX_NS));
    }
    return request_interval_ns;
}

uint64_t retransmit_inflight_grace_ns_locked(const ClientState& state) {
    uint64_t inflight_ns =
        clamp_timer_ns(state.retx_inflight_grace_ns, WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS, WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS);
    uint64_t gap_ns = udp_gap_pressure_ns(state);
    if (gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        inflight_ns =
            std::max(inflight_ns, clamp_timer_ns(gap_ns * 2ull, WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS, WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS));
    }
    return inflight_ns;
}

uint64_t summary_clean_interval_ns_locked(const ClientState& state) {
    return clamp_timer_ns(ms_to_ns(state.config.clean_summary_interval_ms, WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS),
                          WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS, WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS);
}

uint64_t large_summary_repair_grace_ns_locked(const ClientState& state) {
    return std::max({summary_retransmit_grace_ns(state), retransmit_request_interval_ns(state), summary_clean_interval_ns_locked(state)});
}

bool recent_concrete_repair_loss_signal(const ClientState& state, uint64_t now_ns) {
    const uint64_t until_ns = state.summary_repair_loss_signal_until_ns.load(std::memory_order_relaxed);
    return until_ns != 0 && now_ns < until_ns;
}

bool large_summary_repair_batch_locked(ClientState& state, uint16_t total_tiles, size_t candidate_count, uint64_t min_candidate_age_ns,
                                       uint64_t now_ns, bool record_suppressed) {
    if (total_tiles == 0 || candidate_count == 0 ||
        candidate_count * 100ull < static_cast<uint64_t>(total_tiles) * WD_LINK_LARGE_SUMMARY_REPAIR_PERCENT)
    {
        return false;
    }

    if (recent_concrete_repair_loss_signal(state, now_ns))
    {
        return false;
    }

    const uint64_t large_grace_ns = large_summary_repair_grace_ns_locked(state);
    const bool     blocked        = min_candidate_age_ns < large_grace_ns ||
                                    (state.summary_large_repair_not_before_ns != 0 && now_ns < state.summary_large_repair_not_before_ns);

    if (blocked)
    {
        if (record_suppressed)
        {
            state.stats.summary_retx_tiles_throttled.fetch_add(candidate_count, std::memory_order_relaxed);
        }
        return true;
    }

    const uint64_t cooldown_ns = std::max<uint64_t>(WD_LINK_LARGE_SUMMARY_REPAIR_COOLDOWN_NS, summary_clean_interval_ns_locked(state));
    state.summary_large_repair_not_before_ns = now_ns + cooldown_ns;
    return false;
}

void apply_link_timers_from_config(ClientState& state, const wd_server_config_payload& config) {
    const uint64_t summary_grace_ns = clamp_timer_ns(ms_to_ns(config.summary_retransmit_grace_ms, WD_LINK_SUMMARY_GRACE_DEFAULT_NS),
                                                     WD_LINK_SUMMARY_GRACE_MIN_NS, WD_LINK_SUMMARY_GRACE_MAX_NS);
    const uint64_t request_interval_ns =
        clamp_timer_ns(ms_to_ns(config.retransmit_request_interval_ms, WD_LINK_RETRANSMIT_REQUEST_INTERVAL_DEFAULT_NS),
                       WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MIN_NS, WD_LINK_RETRANSMIT_REQUEST_INTERVAL_MAX_NS);
    const uint64_t inflight_ns      = clamp_timer_ns(ms_to_ns(config.retransmit_inflight_grace_ms, WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS),
                                                     WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS, WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS);
    const uint64_t reassembly_ns    = clamp_timer_ns(ms_to_ns(config.tile_reassembly_timeout_ms, WD_LINK_TILE_REASSEMBLY_DEFAULT_NS),
                                                     WD_LINK_TILE_REASSEMBLY_MIN_NS, WD_LINK_TILE_REASSEMBLY_MAX_NS);
    const uint64_t reassembly_floor_ns = clamp_timer_ns(std::max<uint64_t>(WD_LINK_TILE_REASSEMBLY_MIN_NS, reassembly_ns / 2),
                                                        WD_LINK_TILE_REASSEMBLY_MIN_NS, WD_LINK_TILE_REASSEMBLY_MAX_NS);

    state.summary_retransmit_grace_ns.store(summary_grace_ns, std::memory_order_relaxed);
    state.retransmit_request_interval_ns.store(request_interval_ns, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(state.retx_mutex);
        state.retx_inflight_grace_ns = inflight_ns;
    }

    {
        std::lock_guard<std::mutex> lock(state.tile_reassembly_timeout_mutex);
        state.tile_reassembly_floor_ns.store(reassembly_floor_ns, std::memory_order_relaxed);
        state.tile_reassembly_timeout_ns.store(reassembly_ns, std::memory_order_relaxed);
        state.tile_reassembly_ewma_ns      = static_cast<double>(reassembly_ns);
        state.tile_reassembly_deviation_ns = static_cast<double>(reassembly_ns) / 4.0;
    }
}

uint64_t next_input_sequence(ClientState& state) {
    uint64_t seq = state.next_input_sequence.fetch_add(1, std::memory_order_relaxed);
    if (seq == 0)
    {
        seq = state.next_input_sequence.fetch_add(1, std::memory_order_relaxed);
    }
    return seq;
}

void remember_input_timestamp(ClientState& state, uint64_t sequence, uint64_t timestamp_ns) {
    if (sequence == 0 || timestamp_ns == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(state.input_timestamp_mutex);
    state.recent_input_timestamps.push_back({sequence, timestamp_ns});
    while (state.recent_input_timestamps.size() > WD_CLIENT_INPUT_TIMESTAMP_HISTORY_ENTRIES)
    {
        state.recent_input_timestamps.pop_front();
    }
}

bool handle_mtu_probe_start(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (payload_size != sizeof(wd_mtu_probe_start_payload))
    {
        return false;
    }

    wd_mtu_probe_start_payload start{};
    std::memcpy(&start, payload, sizeof(start));

    uint16_t              max_received = 0;
    const uint64_t        start_ns     = wd_now_ns();
    const uint64_t        deadline_ns  = start_ns + WD_NET_MTU_PROBE_CLIENT_DEADLINE_NS;
    std::vector<uint64_t> probe_offsets_ns;
    probe_offsets_ns.reserve(start.probe_count);

    std::vector<uint8_t> recvbuf(WD_UDP_TILE_HEADER_MAX_SIZE + UINT16_MAX);

    while (wd_now_ns() < deadline_ns)
    {
        ssize_t n = ::recv(state.session.transport.udp_fd, recvbuf.data(), recvbuf.size(), 0);

        if (n < 0)
        {
            if (errno == EAGAIN)
            {
                usleep(WD_NET_PROBE_RETRY_SLEEP_MS * WD_USEC_PER_MSEC);
                continue;
            }

            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        if (static_cast<size_t>(n) < WD_UDP_TILE_HEADER_MIN_SIZE)
        {
            continue;
        }

        wd_udp_tile_packet_decoded h{};
        if (!wd_udp_tile_packet_decode(recvbuf.data(), static_cast<size_t>(n), &h))
        {
            continue;
        }

        if (h.tile_id != WD_UDP_TILE_ID_MTU_PROBE)
        {
            continue;
        }

        if (h.session_id != start.session_id || h.connection_token != start.connection_token || h.tile_pkt_count != start.probe_count ||
            h.tile_pkt_id >= start.probe_count)
        {
            continue;
        }

        if (h.tile_payload_size != h.payload_size)
        {
            continue;
        }

        if (static_cast<size_t>(n) != (size_t)h.header_size + h.payload_size)
        {
            continue;
        }

        const uint64_t rx_ns     = wd_now_ns();
        uint64_t       offset_ns = rx_ns - start_ns;
        if (offset_ns > MTU_PROBE_SERVER_STARTUP_DELAY_NS)
        {
            offset_ns -= MTU_PROBE_SERVER_STARTUP_DELAY_NS;
        }
        probe_offsets_ns.push_back(offset_ns);

        if (h.payload_size > max_received)
        {
            max_received = h.payload_size;
        }
    }

    if (!probe_offsets_ns.empty())
    {
        double mean_ns = 0.0;
        for (uint64_t sample : probe_offsets_ns)
        {
            mean_ns += static_cast<double>(sample);
        }
        mean_ns /= static_cast<double>(probe_offsets_ns.size());

        double variance_ns = 0.0;
        for (uint64_t sample : probe_offsets_ns)
        {
            const double delta = static_cast<double>(sample) - mean_ns;
            variance_ns += delta * delta;
        }
        variance_ns /= static_cast<double>(probe_offsets_ns.size());

        const double stddev_ns       = std::sqrt(variance_ns);
        state.retx_inflight_grace_ns = clamp_retransmit_grace_ns(static_cast<uint64_t>(mean_ns + 2.0 * stddev_ns));
    }
    else
    {
        state.retx_inflight_grace_ns = RETRANSMIT_GRACE_DEFAULT_NS;
    }

    if (max_received == 0)
    {
        max_received = WD_UDP_PAYLOAD_TARGET;
    }

    wd_mtu_probe_result_payload result{};
    result.session_id               = start.session_id;
    result.connection_token         = start.connection_token;
    result.max_udp_payload_received = max_received;

    return wd_send_tcp_message(state.session.transport.control_fd, WD_MSG_MTU_PROBE_RESULT, &result, sizeof(result));
}

bool handle_throughput_probe_start(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (payload_size != sizeof(wd_throughput_probe_start_payload))
    {
        return false;
    }

    wd_throughput_probe_start_payload start{};
    std::memcpy(&start, payload, sizeof(start));

    if (start.probe_count == 0 || start.probe_count > UINT8_MAX || start.payload_size == 0)
    {
        return false;
    }

    const bool duration_limited = start.probe_count == UINT8_MAX;

    uint64_t       bytes_received   = 0;
    uint32_t       packets_received = 0;
    const uint64_t start_ns         = wd_now_ns();
    const uint64_t deadline_ns =
        start_ns + (static_cast<uint64_t>(start.duration_ms) + WD_NET_THROUGHPUT_DEADLINE_PADDING_MS) * WD_NSEC_PER_MSEC;

    std::vector<uint8_t> recvbuf(WD_UDP_TILE_HEADER_MAX_SIZE + UINT16_MAX);

    while (wd_now_ns() < deadline_ns && (duration_limited || packets_received < start.probe_count))
    {
        ssize_t n = ::recv(state.session.transport.udp_fd, recvbuf.data(), recvbuf.size(), 0);

        if (n < 0)
        {
            if (errno == EAGAIN)
            {
                usleep(WD_NET_PROBE_RETRY_SLEEP_MS * WD_USEC_PER_MSEC);
                continue;
            }

            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        if (static_cast<size_t>(n) < WD_UDP_TILE_HEADER_MIN_SIZE)
        {
            continue;
        }

        wd_udp_tile_packet_decoded h{};
        if (!wd_udp_tile_packet_decode(recvbuf.data(), static_cast<size_t>(n), &h))
        {
            continue;
        }

        if (h.tile_id != WD_UDP_TILE_ID_THROUGHPUT_PROBE)
        {
            continue;
        }

        if (h.session_id != start.session_id || h.connection_token != start.connection_token || h.tile_pkt_count != start.probe_count ||
            h.tile_pkt_id >= start.probe_count)
        {
            continue;
        }

        if (h.payload_size != start.payload_size || h.tile_payload_size != h.payload_size)
        {
            continue;
        }

        if (static_cast<size_t>(n) != (size_t)h.header_size + h.payload_size)
        {
            continue;
        }

        bytes_received += static_cast<uint64_t>(n);
        packets_received++;
    }

    uint16_t duration_ms = start.duration_ms;
    if (duration_ms == 0)
    {
        duration_ms = 1;
    }

    wd_throughput_probe_result_payload result{};
    result.session_id       = start.session_id;
    result.connection_token = start.connection_token;
    result.bytes_received   = bytes_received;
    result.packets_received = packets_received;
    result.duration_ms      = duration_ms;

    return wd_send_tcp_message(state.session.transport.control_fd, WD_MSG_THROUGHPUT_PROBE_RESULT, &result, sizeof(result));
}

bool handle_link_probe_ping(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (!payload || payload_size != sizeof(wd_link_probe_payload))
    {
        return false;
    }

    wd_link_probe_payload pong{};
    std::memcpy(&pong, payload, sizeof(pong));
    return wd_send_tcp_message(state.session.transport.control_fd, WD_MSG_LINK_PROBE_PONG, &pong, sizeof(pong));
}

bool receive_server_config(ClientState& state) {
    wd_client_hello_payload hello{};
    hello.client_udp_port                  = state.client_udp_port;
    hello.requested_capture_fps            = state.stream_config.target_fps;
    hello.desired_width                    = state.desired_width;
    hello.desired_height                   = state.desired_height;
    hello.udp_rate_cap_kib_per_second       = state.stream_config.udp_rate_cap_kib_per_second;
    const bool     video_allowed           = state.stream_config.video_mode != WD_VIDEO_MODE_OFF;
    const uint32_t supported_video_codecs  = client_video_decoder_supported_codecs(state.session.video_decoder);
    const uint32_t requested_video_codecs  = state.stream_config.video_codec_mask & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265);
    const uint32_t advertised_video_codecs = video_allowed ? (supported_video_codecs & requested_video_codecs) : 0;
    const bool     video_decoder_available = advertised_video_codecs != 0;
    const bool     audio_available = !state.stream_config.disable_audio && state.session.audio_playback && client_audio_playback_available();
    hello.capabilities             = video_decoder_available ? WD_CLIENT_CAP_VIDEO_STREAM : 0;
    if (audio_available)
    {
        hello.capabilities |= WD_CLIENT_CAP_AUDIO_STREAM;
        hello.audio_codecs            = WD_AUDIO_CODEC_OPUS;
        hello.audio_transport         = WD_AUDIO_TRANSPORT_TCP;
        hello.audio_max_channels      = WD_AUDIO_CHANNELS_MAX;
        hello.audio_target_latency_ms = WD_AUDIO_TARGET_LATENCY_MS_DEFAULT;
    }
    hello.video_codecs                 = advertised_video_codecs;
    hello.video_transport              = video_decoder_available ? WD_VIDEO_TRANSPORT_TCP : 0;
    hello.video_mode                   = state.stream_config.video_mode;
    hello.video_min_dirty_percent      = state.stream_config.video_min_dirty_percent;
    hello.video_enter_seconds          = state.stream_config.video_enter_seconds;
    hello.video_bitrate_kib_per_second = state.stream_config.video_bitrate_kib_per_second;
    hello.video_exit_dirty_percent     = state.stream_config.video_exit_dirty_percent;
    hello.video_exit_seconds           = state.stream_config.video_exit_seconds;

    WD_LOG_INFO(
        "video mode control: mode=%s codec=%s bitrate_kib=%u min_dirty_pct=%u enter_seconds=%u exit_dirty_pct=%u exit_seconds=%u "
        "hwdecode=%s decoder=%s",
        video_mode_name(state.stream_config.video_mode), video_codec_name(requested_video_codecs),
        static_cast<unsigned>(state.stream_config.video_bitrate_kib_per_second),
        static_cast<unsigned>(state.stream_config.video_min_dirty_percent), static_cast<unsigned>(state.stream_config.video_enter_seconds),
        static_cast<unsigned>(state.stream_config.video_exit_dirty_percent), static_cast<unsigned>(state.stream_config.video_exit_seconds),
        video_hwdecode_mode_name(state.stream_config.video_hwdecode_mode), video_decoder_available ? "yes" : "no");
    WD_LOG_INFO("audio mode: requested=%s backend=%s codec=%s transport=%s target_latency_ms=%u",
                state.stream_config.disable_audio ? "disabled" : "enabled", client_audio_playback_backend_name(),
                audio_available ? "opus" : "none", audio_available ? "tcp" : "none", WD_AUDIO_TARGET_LATENCY_MS_DEFAULT);

    if (!wd_send_tcp_message(state.session.transport.control_fd, WD_MSG_CLIENT_HELLO, &hello, sizeof(hello)))
    {
        WD_LOG_ERROR("failed to send CLIENT_HELLO");
        return false;
    }

    for (;;)
    {
        uint16_t message_type = 0;
        uint8_t* payload      = nullptr;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(state.session.transport.control_fd, &message_type, &payload, &payload_size))
        {
            WD_LOG_ERROR("failed to receive SERVER_CONFIG");
            return false;
        }

        if (!wd_protocol_message_allowed(message_type, WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_NEGOTIATION,
                                         WD_PROTOCOL_SERVER_TO_CLIENT, payload_size))
        {
            WD_LOG_ERROR("rejected negotiation message=%s(%u) size=%u", wd_protocol_message_name(message_type), message_type,
                         payload_size);
            std::free(payload);
            return false;
        }

        if (message_type == WD_MSG_MTU_PROBE_START)
        {
            const bool ok = handle_mtu_probe_start(state, payload, payload_size);
            std::free(payload);

            if (!ok)
            {
                WD_LOG_ERROR("failed UDP MTU probe");
                return false;
            }

            continue;
        }

        if (message_type == WD_MSG_THROUGHPUT_PROBE_START)
        {
            const bool ok = handle_throughput_probe_start(state, payload, payload_size);
            std::free(payload);

            if (!ok)
            {
                WD_LOG_ERROR("failed UDP throughput probe");
                return false;
            }

            continue;
        }

        if (message_type == WD_MSG_LINK_PROBE_PING)
        {
            const bool ok = handle_link_probe_ping(state, payload, payload_size);
            std::free(payload);

            if (!ok)
            {
                WD_LOG_ERROR("failed TCP link probe");
                return false;
            }

            continue;
        }

        if (message_type == WD_MSG_SERVER_CONFIG && payload_size == sizeof(wd_server_config_payload))
        {
            std::memcpy(&state.config, payload, sizeof(state.config));
            std::free(payload);
            break;
        }

        WD_LOG_ERROR("unexpected TCP message while waiting for SERVER_CONFIG: %u", message_type);
        std::free(payload);
        return false;
    }

    ClientConfigValidationError config_error{};
    if (!client_normalize_and_validate_server_config(state.config, &config_error))
    {
        WD_LOG_ERROR("invalid or unsupported server config: reason=%s display=%ux%u tile=%ux%u grid=%ux%u total=%u udp=%u",
                     client_config_validation_error_name(config_error), state.config.width, state.config.height, state.config.tile_width,
                     state.config.tile_height, state.config.tiles_x, state.config.tiles_y, state.config.total_tiles,
                     state.config.udp_payload_target);
        return false;
    }

    apply_link_timers_from_config(state, state.config);
    state.media_clock_id              = state.config.media_clock_id;
    state.media_clock_local_origin_ns = wd_now_ns();

    state.video_stream_negotiated = (state.config.capabilities & WD_SERVER_CAP_VIDEO_STREAM) != 0 &&
                                    (state.config.video_codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265)) != 0 &&
                                    state.config.video_transport == WD_VIDEO_TRANSPORT_TCP;
    state.video_codecs            = state.video_stream_negotiated ? state.config.video_codecs : 0;
    state.video_transport         = state.video_stream_negotiated ? state.config.video_transport : 0;
    state.audio_stream_negotiated = !state.stream_config.disable_audio && (state.config.capabilities & WD_SERVER_CAP_AUDIO_STREAM) != 0 &&
                                    state.config.audio_codec == WD_AUDIO_CODEC_OPUS &&
                                    state.config.audio_transport == WD_AUDIO_TRANSPORT_TCP && state.session.audio_playback != nullptr;
    state.audio_codec             = state.audio_stream_negotiated ? state.config.audio_codec : 0;
    state.audio_transport         = state.audio_stream_negotiated ? state.config.audio_transport : 0;
    state.audio_channels          = state.audio_stream_negotiated ? state.config.audio_channels : 0;
    state.audio_target_latency_ms = state.audio_stream_negotiated ? state.config.audio_target_latency_ms : 0;

    WD_LOG_INFO("UDP payload target: %u", state.config.udp_payload_target);
    WD_LOG_INFO("video stream negotiation: %s codec=%s transport=%s", state.video_stream_negotiated ? "enabled" : "unavailable",
                state.video_stream_negotiated ? video_codec_name(state.video_codecs) : "none",
                state.video_stream_negotiated ? "tcp" : "none");
    WD_LOG_INFO("audio stream negotiation: %s codec=%s transport=%s rate=%u channels=%u target_latency_ms=%u",
                state.audio_stream_negotiated ? "enabled" : (state.stream_config.disable_audio ? "disabled by client" : "unavailable"),
                state.audio_stream_negotiated ? "opus" : "none", state.audio_stream_negotiated ? "tcp" : "none",
                state.audio_stream_negotiated ? state.config.audio_sample_rate : 0, state.audio_channels, state.audio_target_latency_ms);
    WD_LOG_INFO("link timers: rtt=%ums summary_grace=%ums request_interval=%ums inflight=%ums reassembly=%ums summary_delta=%u/%ums",
                state.config.link_rtt_ms, state.config.summary_retransmit_grace_ms, state.config.retransmit_request_interval_ms,
                state.config.retransmit_inflight_grace_ms, state.config.tile_reassembly_timeout_ms, state.config.active_summary_interval_ms,
                state.config.clean_summary_interval_ms);

    return true;
}

struct SummaryRepairCandidate {
    uint16_t tile_id;
    uint64_t generation;
    uint64_t pending_since_ns;
};

void ensure_retransmit_tracking_locked(ClientState& state, uint16_t total_tiles) {
    if (state.retx_queued_generation.size() != total_tiles)
    {
        state.retx_queued_generation.assign(total_tiles, 0);
    }
    if (state.retx_last_requested_generation.size() != total_tiles)
    {
        state.retx_last_requested_generation.assign(total_tiles, 0);
    }
    if (state.retx_last_request_ns.size() != total_tiles)
    {
        state.retx_last_request_ns.assign(total_tiles, 0);
    }
    if (state.retx_inflight_generation.size() != total_tiles)
    {
        state.retx_inflight_generation.assign(total_tiles, 0);
    }
    if (state.retx_inflight_since_ns.size() != total_tiles)
    {
        state.retx_inflight_since_ns.assign(total_tiles, 0);
    }

    bool reset_summary_pending = false;
    if (state.retx_summary_pending_generation.size() != total_tiles)
    {
        state.retx_summary_pending_generation.assign(total_tiles, 0);
        reset_summary_pending = true;
    }
    if (state.retx_summary_pending_since_ns.size() != total_tiles)
    {
        state.retx_summary_pending_since_ns.assign(total_tiles, 0);
        reset_summary_pending = true;
    }
    if (state.retx_summary_pending_position.size() != total_tiles)
    {
        state.retx_summary_pending_position.assign(total_tiles, UINT32_MAX);
        reset_summary_pending = true;
    }
    if (reset_summary_pending)
    {
        state.retx_summary_pending_tiles.clear();
        state.retx_summary_due_queue.clear();
        state.retx_summary_pending_count = 0;
    }
}

void clear_summary_pending_locked(ClientState& state, uint16_t tile_id) {
    if (tile_id >= state.retx_summary_pending_generation.size() || tile_id >= state.retx_summary_pending_since_ns.size() ||
        tile_id >= state.retx_summary_pending_position.size())
    {
        return;
    }

    (void)summary_pending_index_remove(state.retx_summary_pending_tiles, state.retx_summary_pending_position, tile_id);

    state.retx_summary_pending_generation[tile_id] = 0;
    state.retx_summary_pending_since_ns[tile_id]   = 0;
    state.retx_summary_pending_count               = static_cast<uint32_t>(state.retx_summary_pending_tiles.size());
}

void schedule_summary_pending_due_locked(ClientState& state, uint16_t tile_id, uint64_t generation, uint64_t due_ns) {
    state.retx_summary_due_queue.push_back({tile_id, generation, due_ns});
}

void set_summary_pending_locked(ClientState& state, uint16_t tile_id, uint64_t generation, uint64_t now_ns) {
    if (generation == 0 || tile_id >= state.retx_summary_pending_generation.size() ||
        tile_id >= state.retx_summary_pending_since_ns.size() || tile_id >= state.retx_summary_pending_position.size() ||
        state.retx_summary_pending_generation[tile_id] >= generation)
    {
        return;
    }

    if (!summary_pending_index_add(state.retx_summary_pending_tiles, state.retx_summary_pending_position, tile_id))
    {
        return;
    }
    state.retx_summary_pending_generation[tile_id] = generation;
    state.retx_summary_pending_since_ns[tile_id]   = now_ns;
    const uint64_t grace_ns                        = summary_retransmit_grace_ns(state);
    schedule_summary_pending_due_locked(state, tile_id, generation, now_ns > UINT64_MAX - grace_ns ? UINT64_MAX : now_ns + grace_ns);
    state.retx_summary_pending_count = static_cast<uint32_t>(state.retx_summary_pending_tiles.size());
}

bool client_repair_pressure_high_locked(const ClientState& state, uint16_t total_tiles) {
    if (total_tiles == 0)
    {
        return false;
    }

    const uint64_t pressure_tiles =
        std::max<uint64_t>(WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES, (uint64_t)total_tiles * WD_CLIENT_REPAIR_PRESSURE_PERCENT / 100ull);
    return state.retx_queue.size() >= pressure_tiles || state.retx_summary_pending_count >= pressure_tiles;
}

size_t limit_repair_candidates_under_pressure_locked(const ClientState& state, uint16_t total_tiles,
                                                     std::vector<SummaryRepairCandidate>& candidates) {
    if (!client_repair_pressure_high_locked(state, total_tiles) || candidates.size() <= WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES)
    {
        return 0;
    }

    /* Generations are per-tile and are not comparable across different tile IDs.
     * Prefer the oldest missing tiles, and leave the remaining candidates in the
     * summary-pending table for the next promotion pass. Clearing them here can
     * strand a static-screen repair until the next full sanity summary. */
    std::sort(candidates.begin(), candidates.end(), [](const SummaryRepairCandidate& a, const SummaryRepairCandidate& b) {
        if (a.pending_since_ns != b.pending_since_ns)
        {
            return a.pending_since_ns < b.pending_since_ns;
        }
        return a.tile_id < b.tile_id;
    });

    const size_t deferred = candidates.size() - WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES;
    candidates.resize(WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES);
    return deferred;
}

bool queue_retransmit_tile_locked(ClientState& state, uint16_t tile_id, uint64_t generation, uint16_t total_tiles) {
    if (tile_id >= total_tiles || generation == 0)
    {
        return false;
    }

    ensure_retransmit_tracking_locked(state, total_tiles);

    if (state.retx_queued_generation[tile_id] == 0)
    {
        state.retx_queue.push_back(tile_id);
    }

    if (state.retx_queued_generation[tile_id] < generation)
    {
        state.retx_queued_generation[tile_id] = generation;
    }

    return true;
}

void queue_retransmits_from_summary(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (payload_size < sizeof(wd_tile_summary_payload_header))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    wd_tile_summary_payload_header summary{};
    std::memcpy(&summary, payload, sizeof(summary));

    const uint64_t now_ns = wd_now_ns();

    if (!wd_counted_payload_size_is_valid(payload_size, sizeof(wd_tile_summary_payload_header), summary.tile_count,
                                          sizeof(wd_tile_generation_entry)) ||
        (summary.flags & ~WD_TILE_SUMMARY_FLAG_MASK) != 0)
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto* entries = reinterpret_cast<const wd_tile_generation_entry*>(payload + sizeof(wd_tile_summary_payload_header));

    uint16_t total_tiles = 0;
    {
        std::lock_guard<std::mutex> config_lock(state.config_mutex);
        if (summary.session_id != state.config.session_id || summary.connection_token != state.config.connection_token)
        {
            return;
        }
        total_tiles = state.config.total_tiles;
    }
    if (!wd_tile_summary_count_is_valid(summary.flags, summary.tile_count, total_tiles))
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (client_accept_content_epoch(state, summary.content_epoch, WD_CLIENT_CONTENT_OWNER_TILES) == ClientContentEpochDecision::Stale)
    {
        return;
    }

    {
        std::scoped_lock generation_retx_lock(state.generation_mutex, state.retx_mutex);

        if (total_tiles == 0 || state.received_generation.size() != total_tiles)
        {
            return;
        }

        ensure_retransmit_tracking_locked(state, total_tiles);

        uint64_t                            newly_deferred_from_summary = 0;
        std::vector<SummaryRepairCandidate> candidates;
        candidates.reserve(summary.tile_count);
        uint64_t min_candidate_age_ns = UINT64_MAX;

        for (uint16_t i = 0; i < summary.tile_count; ++i)
        {
            const wd_tile_generation_entry& entry = entries[i];

            if (entry.tile_id >= total_tiles)
            {
                continue;
            }

            if (entry.tile_generation <= state.received_generation[entry.tile_id])
            {
                clear_summary_pending_locked(state, entry.tile_id);
                continue;
            }

            if (state.retx_inflight_generation[entry.tile_id] >= entry.tile_generation &&
                state.retx_inflight_since_ns[entry.tile_id] != 0 &&
                now_ns - state.retx_inflight_since_ns[entry.tile_id] < retransmit_inflight_grace_ns_locked(state))
            {
                if (state.retx_summary_pending_generation[entry.tile_id] < entry.tile_generation)
                {
                    set_summary_pending_locked(state, entry.tile_id, entry.tile_generation, now_ns);
                    newly_deferred_from_summary++;
                }
                continue;
            }

            if (state.retx_last_requested_generation[entry.tile_id] >= entry.tile_generation &&
                state.retx_last_request_ns[entry.tile_id] != 0 &&
                now_ns - state.retx_last_request_ns[entry.tile_id] < retransmit_request_interval_ns(state))
            {
                if (state.retx_summary_pending_generation[entry.tile_id] < entry.tile_generation)
                {
                    set_summary_pending_locked(state, entry.tile_id, entry.tile_generation, now_ns);
                    newly_deferred_from_summary++;
                }
                continue;
            }

            /*
             * Summaries are sent immediately after tile sends, and TCP can
             * easily beat the corresponding UDP tile packets to the client.
             * Treat the first sighting of a newer generation as an in-flight
             * hint, not proof of loss. Queue repair only after a short local
             * grace period or from a partial-tile timeout.
             */
            if (state.retx_summary_pending_generation[entry.tile_id] < entry.tile_generation)
            {
                set_summary_pending_locked(state, entry.tile_id, entry.tile_generation, now_ns);
                newly_deferred_from_summary++;
                continue;
            }

            const uint64_t pending_since_ns = state.retx_summary_pending_since_ns[entry.tile_id];
            if (pending_since_ns != 0 && now_ns - pending_since_ns < summary_retransmit_grace_ns(state))
            {
                continue;
            }

            const uint64_t candidate_age_ns = pending_since_ns != 0 && now_ns >= pending_since_ns ? now_ns - pending_since_ns : 0;
            min_candidate_age_ns            = std::min(min_candidate_age_ns, candidate_age_ns);
            candidates.push_back({entry.tile_id, entry.tile_generation, pending_since_ns});
        }

        if (newly_deferred_from_summary != 0)
        {
            state.stats.summary_retx_tiles_deferred.fetch_add(newly_deferred_from_summary, std::memory_order_relaxed);
        }

        if (candidates.empty())
        {
            return;
        }

        const size_t pressure_deferred = limit_repair_candidates_under_pressure_locked(state, total_tiles, candidates);
        if (pressure_deferred != 0)
        {
            state.stats.summary_retx_pressure_dropped.fetch_add(pressure_deferred, std::memory_order_relaxed);
        }

        if (large_summary_repair_batch_locked(state, total_tiles, candidates.size(), min_candidate_age_ns, now_ns, true))
        {
            return;
        }

        uint64_t newly_queued_from_summary     = 0;
        uint64_t summary_to_retx_local_sum_ns  = 0;
        uint64_t summary_to_retx_local_samples = 0;

        for (const SummaryRepairCandidate& candidate : candidates)
        {
            if (queue_retransmit_tile_locked(state, candidate.tile_id, candidate.generation, total_tiles))
            {
                if (candidate.pending_since_ns != 0 && now_ns >= candidate.pending_since_ns)
                {
                    summary_to_retx_local_sum_ns += now_ns - candidate.pending_since_ns;
                    summary_to_retx_local_samples++;
                }
                clear_summary_pending_locked(state, candidate.tile_id);
                newly_queued_from_summary++;
            }
        }

        if (newly_queued_from_summary != 0)
        {
            state.stats.summary_retx_tiles_queued.fetch_add(newly_queued_from_summary, std::memory_order_relaxed);
            if (summary_to_retx_local_samples != 0)
            {
                state.stats.summary_to_retx_samples.fetch_add(summary_to_retx_local_samples, std::memory_order_relaxed);
                state.stats.summary_to_retx_sum_ns.fetch_add(summary_to_retx_local_sum_ns, std::memory_order_relaxed);
            }
        }
    }
}

void promote_deferred_summary_retransmits_locked(ClientState& state) {
    std::scoped_lock config_generation_retx_lock(state.config_mutex, state.generation_mutex, state.retx_mutex);

    const uint16_t total_tiles = state.config.total_tiles;
    if (total_tiles == 0 || state.received_generation.size() != total_tiles)
    {
        return;
    }

    ensure_retransmit_tracking_locked(state, total_tiles);
    if (state.retx_summary_pending_count == 0 || state.retx_summary_due_queue.empty())
    {
        return;
    }

    state.stats.summary_promote_passes.fetch_add(1, std::memory_order_relaxed);
    const uint64_t                      now_ns = wd_now_ns();
    std::vector<SummaryRepairCandidate> candidates;
    uint64_t                            min_candidate_age_ns = UINT64_MAX;
    uint64_t                            scanned              = 0;

    while (!state.retx_summary_due_queue.empty() && state.retx_summary_due_queue.front().due_ns <= now_ns)
    {
        const ClientSummaryRepairDue due = state.retx_summary_due_queue.front();
        state.retx_summary_due_queue.pop_front();
        scanned++;

        if (due.tile_id >= total_tiles || state.retx_summary_pending_generation[due.tile_id] != due.generation)
        {
            continue;
        }

        const uint64_t since_ns = state.retx_summary_pending_since_ns[due.tile_id];
        if (since_ns == 0)
        {
            continue;
        }

        if (state.received_generation[due.tile_id] >= due.generation)
        {
            clear_summary_pending_locked(state, due.tile_id);
            state.stats.summary_retx_tiles_stale_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        uint64_t retry_due_ns = 0;
        if (state.retx_inflight_generation[due.tile_id] >= due.generation && state.retx_inflight_since_ns[due.tile_id] != 0)
        {
            const uint64_t grace_ns = retransmit_inflight_grace_ns_locked(state);
            retry_due_ns            = state.retx_inflight_since_ns[due.tile_id] > UINT64_MAX - grace_ns
                                          ? UINT64_MAX
                                          : state.retx_inflight_since_ns[due.tile_id] + grace_ns;
        }
        if (state.retx_last_requested_generation[due.tile_id] >= due.generation && state.retx_last_request_ns[due.tile_id] != 0)
        {
            const uint64_t request_interval_ns   = retransmit_request_interval_ns(state);
            const uint64_t request_due_ns = state.retx_last_request_ns[due.tile_id] > UINT64_MAX - request_interval_ns
                                                ? UINT64_MAX
                                                : state.retx_last_request_ns[due.tile_id] + request_interval_ns;
            retry_due_ns                  = std::max(retry_due_ns, request_due_ns);
        }
        if (retry_due_ns > now_ns)
        {
            schedule_summary_pending_due_locked(state, due.tile_id, due.generation, retry_due_ns);
            continue;
        }

        const uint64_t candidate_age_ns = now_ns >= since_ns ? now_ns - since_ns : 0;
        min_candidate_age_ns            = std::min(min_candidate_age_ns, candidate_age_ns);
        candidates.push_back({due.tile_id, due.generation, since_ns});
    }

    state.stats.summary_promote_scanned.fetch_add(scanned, std::memory_order_relaxed);
    if (candidates.empty())
    {
        return;
    }

    state.stats.summary_promote_candidates.fetch_add(candidates.size(), std::memory_order_relaxed);

    if (client_repair_pressure_high_locked(state, total_tiles) && candidates.size() > WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES)
    {
        std::sort(candidates.begin(), candidates.end(), [](const SummaryRepairCandidate& a, const SummaryRepairCandidate& b) {
            if (a.pending_since_ns != b.pending_since_ns)
            {
                return a.pending_since_ns < b.pending_since_ns;
            }
            return a.tile_id < b.tile_id;
        });
        const size_t keep = WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES;
        for (size_t i = keep; i < candidates.size(); ++i)
        {
            schedule_summary_pending_due_locked(state, candidates[i].tile_id, candidates[i].generation,
                                                now_ns + WD_NET_CLIENT_REPAIR_RETRY_MIN_NS);
        }
        state.stats.summary_retx_pressure_dropped.fetch_add(candidates.size() - keep, std::memory_order_relaxed);
        candidates.resize(keep);
    }

    if (large_summary_repair_batch_locked(state, total_tiles, candidates.size(), min_candidate_age_ns, now_ns, true))
    {
        const uint64_t retry_ns = std::max<uint64_t>(now_ns + WD_NET_CLIENT_REPAIR_RETRY_MIN_NS, state.summary_large_repair_not_before_ns);
        for (const SummaryRepairCandidate& candidate : candidates)
        {
            schedule_summary_pending_due_locked(state, candidate.tile_id, candidate.generation, retry_ns);
        }
        return;
    }

    uint64_t newly_queued                  = 0;
    uint64_t summary_to_retx_local_sum_ns  = 0;
    uint64_t summary_to_retx_local_samples = 0;

    for (const SummaryRepairCandidate& candidate : candidates)
    {
        if (queue_retransmit_tile_locked(state, candidate.tile_id, candidate.generation, total_tiles))
        {
            if (now_ns >= candidate.pending_since_ns)
            {
                summary_to_retx_local_sum_ns += now_ns - candidate.pending_since_ns;
                summary_to_retx_local_samples++;
            }
            clear_summary_pending_locked(state, candidate.tile_id);
            newly_queued++;
        }
    }

    if (newly_queued != 0)
    {
        state.stats.summary_retx_tiles_queued.fetch_add(newly_queued, std::memory_order_relaxed);
        if (summary_to_retx_local_samples != 0)
        {
            state.stats.summary_to_retx_samples.fetch_add(summary_to_retx_local_samples, std::memory_order_relaxed);
            state.stats.summary_to_retx_sum_ns.fetch_add(summary_to_retx_local_sum_ns, std::memory_order_relaxed);
        }
    }
}

bool selection_payload_to_string(const uint8_t* payload, uint32_t payload_size, uint8_t expected_session_id,
                                 uint64_t expected_connection_token, std::string& out) {
    wd_selection_text_view view{};
    if (!wd_selection_payload_decode(payload, payload_size, expected_session_id, expected_connection_token, &view))
    {
        return false;
    }

    out.assign(reinterpret_cast<const char*>(view.data), view.size);
    return true;
}

bool store_selection_text(ClientState& state, const uint8_t* payload, uint32_t payload_size, bool primary) {
    uint8_t  session_id       = 0;
    uint64_t connection_token = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id       = state.config.session_id;
        connection_token = state.config.connection_token;
    }

    std::string text;
    if (!selection_payload_to_string(payload, payload_size, session_id, connection_token, text))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state.selection_mutex);
        if (primary)
        {
            state.pending_primary_text       = std::move(text);
            state.pending_primary_text_valid = true;
        }
        else
        {
            state.pending_clipboard_text       = std::move(text);
            state.pending_clipboard_text_valid = true;
        }
    }
    state.render_wake.signal();
    return true;
}

void store_cursor_shape(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (!payload || payload_size != sizeof(wd_cursor_shape_payload))
    {
        return;
    }

    wd_cursor_shape_payload cursor{};
    std::memcpy(&cursor, payload, sizeof(cursor));

    uint8_t  session_id       = 0;
    uint64_t connection_token = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id       = state.config.session_id;
        connection_token = state.config.connection_token;
    }

    if (cursor.session_id == session_id && cursor.connection_token == connection_token && cursor.shape < WD_CURSOR_SHAPE_COUNT)
    {
        state.pending_cursor_shape.store(cursor.shape, std::memory_order_relaxed);
        state.pending_cursor_shape_dirty.store(true, std::memory_order_release);
        state.render_wake.signal();
    }
}

void discard_pending_video_frame(ClientState& state) {
    bool wake_render = false;
    {
        std::scoped_lock dirty_video_lock(state.dirty_rect_mutex, state.video_frame_mutex);
        const uint64_t              tile_epoch = wd_client_stream_ownership_end_video_stream(&state.stream_ownership);
        state.video_present_queue.clear();
        state.pending_video_frame_dirty.store(false, std::memory_order_release);
        if (state.pending_dirty_tiles.dirty_tile_count() != 0)
        {
            state.pending_dirty_epoch = tile_epoch;
            wake_render               = true;
        }
    }
    if (wake_render)
    {
        state.render_wake.signal();
    }
}

void reset_video_decoder(ClientState& state, const char* reason,
                         enum wd_client_video_phase next_phase = WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME) {
    std::lock_guard<std::mutex> lock(state.session.video_decoder_mutex);
    client_video_decoder_reset(state.session.video_decoder);
    state.session.video_decoder_needs_keyframe = next_phase != WD_CLIENT_VIDEO_PHASE_VIDEO;
    state.session.video_phase                  = next_phase;
    state.stats.video_decoder_resets.fetch_add(1, std::memory_order_relaxed);
    state.stats.video_last_frame_id_rx.store(0, std::memory_order_relaxed);
    state.stats.video_last_frame_id_presented.store(0, std::memory_order_relaxed);
    if (reason)
    {
        WD_LOG_INFO("video decoder reset: reason=%s", reason);
    }
}

void store_server_config_update(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (!payload || payload_size != sizeof(wd_server_config_payload))
    {
        return;
    }

    wd_server_config_payload config{};
    std::memcpy(&config, payload, sizeof(config));

    ClientConfigValidationError config_error{};
    if (!client_normalize_and_validate_server_config(config, &config_error))
    {
        WD_LOG_ERROR("ignoring invalid server config update: reason=%s display=%ux%u tile=%ux%u grid=%ux%u total=%u udp=%u",
                     client_config_validation_error_name(config_error), config.width, config.height, config.tile_width, config.tile_height,
                     config.tiles_x, config.tiles_y, config.total_tiles, config.udp_payload_target);
        return;
    }

    apply_link_timers_from_config(state, config);

    const bool     new_video_stream_negotiated = (config.capabilities & WD_SERVER_CAP_VIDEO_STREAM) != 0 &&
                                                 (config.video_codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265)) != 0 &&
                                                 config.video_transport == WD_VIDEO_TRANSPORT_TCP;
    const uint32_t new_video_codecs            = new_video_stream_negotiated ? config.video_codecs : 0;
    const uint16_t new_video_transport         = new_video_stream_negotiated ? config.video_transport : 0;

    bool reset_video = false;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        const bool                  same_connection =
            state.config.session_id == config.session_id && state.config.connection_token == config.connection_token;
        uint64_t                        newest_config_epoch = same_connection ? state.config.config_epoch : 0;
        const wd_server_config_payload* same_epoch_config   = same_connection ? &state.config : nullptr;
        if (state.pending_config_valid && state.pending_config.session_id == config.session_id &&
            state.pending_config.connection_token == config.connection_token)
        {
            if (state.pending_config.config_epoch > newest_config_epoch)
            {
                newest_config_epoch = state.pending_config.config_epoch;
                same_epoch_config   = &state.pending_config;
            }
            else if (state.pending_config.config_epoch == newest_config_epoch)
            {
                same_epoch_config = &state.pending_config;
            }
        }
        if (config.config_epoch < newest_config_epoch)
        {
            WD_LOG_DEBUG("ignoring stale server config epoch=%llu newest=%llu", static_cast<unsigned long long>(config.config_epoch),
                         static_cast<unsigned long long>(newest_config_epoch));
            return;
        }
        if (config.config_epoch == newest_config_epoch && same_epoch_config && std::memcmp(same_epoch_config, &config, sizeof(config)) != 0)
        {
            WD_LOG_ERROR("rejecting conflicting server config epoch=%llu", static_cast<unsigned long long>(config.config_epoch));
            return;
        }

        const bool have_current_config = state.config.session_id != 0;
        reset_video = have_current_config &&
                      (state.config.session_id != config.session_id || state.config.connection_token != config.connection_token ||
                       state.config.width != config.width || state.config.height != config.height ||
                       state.video_codecs != new_video_codecs || state.video_transport != new_video_transport);

        state.video_stream_negotiated = new_video_stream_negotiated;
        state.video_codecs            = new_video_codecs;
        state.video_transport         = new_video_transport;
        state.pending_config          = config;
        state.pending_config_valid    = true;
    }

    state.render_wake.signal();
    if (reset_video)
    {
        reset_video_decoder(state, "server config update");
    }
}

bool video_payload_to_packet(ClientState& state, const uint8_t* payload, uint32_t payload_size, ClientVideoPacket& packet,
                             bool& control_frame) {
    control_frame = false;
    if (!payload || payload_size < sizeof(wd_video_frame_payload_header))
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::memcpy(&packet.header, payload, sizeof(packet.header));
    if ((packet.header.codec != WD_VIDEO_CODEC_H265 && packet.header.codec != WD_VIDEO_CODEC_H264) ||
        !wd_video_frame_payload_size_is_valid(&packet.header, payload_size))
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    control_frame = (packet.header.flags & (WD_VIDEO_FRAME_END_OF_STREAM | WD_VIDEO_FRAME_RESIZE)) != 0;
    if (control_frame)
    {
        state.stats.video_control_frames_rx.fetch_add(1, std::memory_order_relaxed);
    }
    if (packet.header.data_size == 0 && !control_frame)
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint8_t  session_id       = 0;
    uint64_t connection_token = 0;
    uint16_t width            = 0;
    uint16_t height           = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id       = state.config.session_id;
        connection_token = state.config.connection_token;
        width            = state.config.width;
        height           = state.config.height;
    }

    if (packet.header.session_id != session_id || packet.header.connection_token != connection_token)
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (((packet.header.width != width || packet.header.height != height) || packet.header.coded_width < packet.header.width ||
         packet.header.coded_height < packet.header.height) &&
        !control_frame)
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        reset_video_decoder(state, "video frame geometry mismatch");
        return false;
    }

    packet.data = wd_video_frame_payload_data(payload, payload_size);
    return true;
}

bool publish_decoded_video_frame(ClientState& state, ClientVideoDecoder* decoder, const ClientDecodedVideoFrame& frame) {
    if (!decoder || frame.format != ClientVideoPixelFormat::IYUV || frame.width == 0 || frame.height == 0)
    {
        return false;
    }

    uint16_t config_width  = 0;
    uint16_t config_height = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        config_width  = state.config.width;
        config_height = state.config.height;
    }

    if (frame.width != config_width || frame.height != config_height)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> content_lock(state.remote_content_mutex);
        if (frame.content_epoch != state.remote_content_epoch || state.remote_content_owner != WD_CLIENT_CONTENT_OWNER_VIDEO)
        {
            return false;
        }
        std::scoped_lock dirty_generation_video_lock(state.dirty_rect_mutex, state.generation_mutex,
                                                        state.video_frame_mutex);

        const struct wd_client_content_ownership_snapshot ownership = wd_client_stream_ownership_snapshot(&state.stream_ownership);
        if (ownership.owner != WD_CLIENT_CONTENT_OWNER_VIDEO)
        {
            return false;
        }

        bool                   dropped_newest = false;
        ClientVideoFrameBuffer decode_buffer  = state.video_present_queue.take_decode_buffer(dropped_newest);
        if (!client_video_decoder_swap_output_frame(decoder, decode_buffer))
        {
            state.video_present_queue.recycle(std::move(decode_buffer));
            return false;
        }
        if (!decode_buffer.valid() || decode_buffer.width != frame.width || decode_buffer.height != frame.height)
        {
            (void)client_video_decoder_swap_output_frame(decoder, decode_buffer);
            state.video_present_queue.recycle(std::move(decode_buffer));
            return false;
        }

        if (!state.video_present_queue.push_decoded(std::move(decode_buffer), frame.width, frame.height, frame.frame_id, frame.pts_usec,
                                                    ownership.epoch))
        {
            return false;
        }
        if (dropped_newest)
        {
            state.stats.video_queue_overflow_drops.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t depth    = state.video_present_queue.size();
        uint64_t       observed = state.stats.video_queue_depth_max.load(std::memory_order_relaxed);
        while (depth > observed && !state.stats.video_queue_depth_max.compare_exchange_weak(observed, depth, std::memory_order_relaxed,
                                                                                            std::memory_order_relaxed))
        {
        }

        state.pending_dirty_tiles.clear();
        state.pending_dirty_rect_count.store(0, std::memory_order_release);
        std::fill(state.pending_present_generation.begin(), state.pending_present_generation.end(), 0);
        state.pending_dirty_epoch = ownership.epoch;
        state.pending_video_frame_dirty.store(true, std::memory_order_release);
    }

    state.render_wake.signal();
    return true;
}

void handle_video_frame(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    ClientVideoPacket packet{};
    bool              control_frame = false;
    if (!video_payload_to_packet(state, payload, payload_size, packet, control_frame))
    {
        return;
    }

    const enum wd_client_content_owner content_owner =
        (packet.header.flags & WD_VIDEO_FRAME_END_OF_STREAM) != 0 ? WD_CLIENT_CONTENT_OWNER_TILES : WD_CLIENT_CONTENT_OWNER_VIDEO;
    const ClientContentEpochDecision content_decision = client_accept_content_epoch(state, packet.header.content_epoch, content_owner);
    if (content_decision == ClientContentEpochDecision::Stale)
    {
        state.stats.video_stale_frames_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool                    resize        = (packet.header.flags & WD_VIDEO_FRAME_RESIZE) != 0;
    const bool                    end_of_stream = (packet.header.flags & WD_VIDEO_FRAME_END_OF_STREAM) != 0;
    const bool                    keyframe      = (packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0;
    struct wd_client_video_transition_decision transition{};
    {
        std::lock_guard<std::mutex> lock(state.session.video_decoder_mutex);
        transition = wd_client_video_transition_decide(
            state.session.video_phase,
            content_decision == ClientContentEpochDecision::Advanced && content_owner == WD_CLIENT_CONTENT_OWNER_VIDEO,
            end_of_stream, resize, keyframe, packet.header.data_size != 0);
        if (transition.reset_decoder)
        {
            client_video_decoder_reset(state.session.video_decoder);
            state.stats.video_decoder_resets.fetch_add(1, std::memory_order_relaxed);
            state.stats.video_last_frame_id_rx.store(0, std::memory_order_relaxed);
            state.stats.video_last_frame_id_presented.store(0, std::memory_order_relaxed);
            WD_LOG_INFO("video decoder reset: reason=%s", resize          ? "video resize"
                                                          : end_of_stream ? "video end-of-stream"
                                                                          : "video content epoch advanced");
        }
        state.session.video_phase                  = transition.next_phase;
        state.session.video_decoder_needs_keyframe = state.session.video_phase != WD_CLIENT_VIDEO_PHASE_VIDEO;
    }

    if (end_of_stream)
    {
        state.session.video_unavailable.store(true, std::memory_order_release);
    }
    if (!transition.accept_payload)
    {
        if (packet.header.data_size != 0 && !keyframe)
        {
            state.stats.video_need_keyframe_drops.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    state.stats.video_data_frames_rx.fetch_add(1, std::memory_order_relaxed);

    const uint64_t last_presented = state.stats.video_last_frame_id_presented.load(std::memory_order_relaxed);
    if (last_presented != 0 && packet.header.frame_id <= last_presented)
    {
        /* A fresh keyframe with a lower ID means the sender restarted its
         * codec/sequence without changing the display session. Recover rather
         * than dropping the restarted stream forever. */
        if ((packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0 && packet.header.frame_id < last_presented)
        {
            reset_video_decoder(state, "video frame id restart");
        }
        else
        {
            state.stats.video_stale_frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    state.stats.video_last_frame_id_rx.store(packet.header.frame_id, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(state.session.video_decoder_mutex);
        const auto                  reset_decoder_locked = [&state](const char* reason) {
            client_video_decoder_reset(state.session.video_decoder);
            state.stats.video_decoder_resets.fetch_add(1, std::memory_order_relaxed);
            state.stats.video_last_frame_id_rx.store(0, std::memory_order_relaxed);
            state.stats.video_last_frame_id_presented.store(0, std::memory_order_relaxed);
            state.session.video_decoder_needs_keyframe = true;
            state.session.video_phase                  = WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME;
            WD_LOG_INFO("video decoder reset: reason=%s", reason);
        };
        if (state.session.video_decoder_needs_keyframe && (packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) == 0)
        {
            state.stats.video_need_keyframe_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ClientVideoDecoderConfig config{};
        config.session_id       = packet.header.session_id;
        config.connection_token = packet.header.connection_token;
        config.content_epoch    = packet.header.content_epoch;
        config.width            = packet.header.width;
        config.height           = packet.header.height;
        config.coded_width      = packet.header.coded_width != 0 ? packet.header.coded_width : packet.header.width;
        config.coded_height     = packet.header.coded_height != 0 ? packet.header.coded_height : packet.header.height;
        config.target_fps       = state.stream_config.target_fps;
        config.codec            = packet.header.codec;
        config.hwdecode_mode    = state.stream_config.video_hwdecode_mode;

        if (!client_video_decoder_configure(state.session.video_decoder, config))
        {
            state.session.video_decoder_needs_keyframe = true;
            state.session.video_phase                  = WD_CLIENT_VIDEO_PHASE_AWAITING_KEYFRAME;
            state.stats.video_decode_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ClientDecodedVideoFrame frame{};
        const uint64_t          decode_start_ns   = wd_now_ns();
        bool                    decoded           = client_video_decoder_decode(state.session.video_decoder, packet, &frame);
        const uint64_t          decode_elapsed_ns = wd_now_ns() - decode_start_ns;
        state.stats.video_decode_sum_ns.fetch_add(decode_elapsed_ns, std::memory_order_relaxed);
        state.stats.video_decode_samples.fetch_add(1, std::memory_order_relaxed);

        if (!decoded && client_video_decoder_hwdecode_failed_auto(state.session.video_decoder))
        {
            /* Auto hardware decode is best-effort. If the VAAPI backend fails
             * while decoding the access unit that unlocks the stream, rebuild
             * the decoder immediately as software and retry that same keyframe
             * instead of waiting for a later periodic keyframe. */
            client_video_decoder_reset(state.session.video_decoder);
            if ((packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0 && client_video_decoder_configure(state.session.video_decoder, config))
            {
                frame                           = ClientDecodedVideoFrame{};
                const uint64_t retry_start_ns   = wd_now_ns();
                decoded                         = client_video_decoder_decode(state.session.video_decoder, packet, &frame);
                const uint64_t retry_elapsed_ns = wd_now_ns() - retry_start_ns;
                state.stats.video_decode_sum_ns.fetch_add(retry_elapsed_ns, std::memory_order_relaxed);
                state.stats.video_decode_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (!decoded)
        {
            state.stats.video_decode_failed.fetch_add(1, std::memory_order_relaxed);
            reset_decoder_locked("video decode failed");
            return;
        }
        bool published_frame = false;
        for (;;)
        {
            if (frame.format != ClientVideoPixelFormat::None)
            {
                state.stats.video_frames_decoded.fetch_add(1, std::memory_order_relaxed);
                if (!publish_decoded_video_frame(state, state.session.video_decoder, frame))
                {
                    state.stats.video_publish_failed.fetch_add(1, std::memory_order_relaxed);
                    reset_decoder_locked("decoded frame publish failed");
                    return;
                }
                published_frame = true;
            }

            frame = ClientDecodedVideoFrame{};
            if (!client_video_decoder_take_frame(state.session.video_decoder, &frame))
            {
                break;
            }
        }

        if (!published_frame)
        {
            return;
        }

        state.session.video_decoder_needs_keyframe = false;
        state.session.video_phase                  = WD_CLIENT_VIDEO_PHASE_VIDEO;
    }

    /* Actual video presentation is recorded by the SDL render thread after
     * SDL_RenderPresent succeeds. Publishing only makes the decoded frame
     * available for upload. */
}

bool process_audio_message(ClientState& state, uint16_t message_type, const uint8_t* payload, uint32_t payload_size) {
    state.stats.audio_messages_rx.fetch_add(1, std::memory_order_relaxed);
    state.stats.audio_bytes_rx.fetch_add(payload_size, std::memory_order_relaxed);

    bool ok = true;
    if (message_type == WD_MSG_AUDIO_CONFIG && payload_size == sizeof(wd_audio_config_payload))
    {
        wd_audio_config_payload config{};
        std::memcpy(&config, payload, sizeof(config));
        uint8_t  session_id = 0;
        uint64_t connection_token = 0;
        uint64_t media_clock_id = 0;
        uint16_t target_latency_ms = 0;
        {
            std::lock_guard<std::mutex> lock(state.config_mutex);
            session_id         = state.config.session_id;
            connection_token   = state.config.connection_token;
            media_clock_id     = state.media_clock_id;
            target_latency_ms  = state.audio_target_latency_ms;
        }
        ok = config.session_id == session_id && config.connection_token == connection_token &&
             config.media_clock_id == media_clock_id &&
             client_audio_playback_configure(state.session.audio_playback, config, target_latency_ms);
    }
    else if (message_type == WD_MSG_AUDIO_PACKET)
    {
        state.stats.audio_packets_rx.fetch_add(1, std::memory_order_relaxed);
        const uint64_t before_discontinuities = client_audio_playback_discontinuities(state.session.audio_playback);
        const uint64_t before_late_drops      = client_audio_playback_late_drops(state.session.audio_playback);
        const uint64_t before_underflows      = client_audio_playback_underflows(state.session.audio_playback);
        ok                                    = client_audio_playback_handle_packet(state.session.audio_playback, payload, payload_size);
        const uint64_t after_discontinuities  = client_audio_playback_discontinuities(state.session.audio_playback);
        const uint64_t after_late_drops       = client_audio_playback_late_drops(state.session.audio_playback);
        const uint64_t after_underflows       = client_audio_playback_underflows(state.session.audio_playback);
        if (after_discontinuities > before_discontinuities)
        {
            state.stats.audio_discontinuities.fetch_add(after_discontinuities - before_discontinuities, std::memory_order_relaxed);
        }
        if (after_late_drops > before_late_drops)
        {
            state.stats.audio_late_drops.fetch_add(after_late_drops - before_late_drops, std::memory_order_relaxed);
        }
        if (after_underflows > before_underflows)
        {
            state.stats.audio_underflows.fetch_add(after_underflows - before_underflows, std::memory_order_relaxed);
        }
    }
    else
    {
        ok = false;
    }

    if (!ok)
    {
        state.stats.audio_decode_failed.fetch_add(1, std::memory_order_relaxed);
        WD_LOG_WARN("invalid audio channel message type=%u size=%u", message_type, payload_size);
    }
    return ok;
}

void release_media_packet(ClientMediaPacket& packet) {
    std::free(packet.payload);
    packet = ClientMediaPacket{};
}

void clear_media_queue(std::deque<ClientMediaPacket>& queue) {
    while (!queue.empty())
    {
        ClientMediaPacket packet = queue.front();
        queue.pop_front();
        release_media_packet(packet);
    }
}

uint16_t video_packet_flags(const wd_tcp_message& message) {
    if (message.payload_size < sizeof(wd_video_frame_payload_header))
    {
        return 0;
    }
    wd_video_frame_payload_header header{};
    std::memcpy(&header, message.payload, sizeof(header));
    return header.flags;
}

bool enqueue_video_message(ClientState& state, wd_tcp_message& message) {
    std::lock_guard<std::mutex> lock(state.session.media_queue_mutex);
    if (!state.session.media_workers_running.load(std::memory_order_acquire))
    {
        return false;
    }

    const uint16_t flags      = video_packet_flags(message);
    const bool     keyframe   = (flags & WD_VIDEO_FRAME_KEYFRAME) != 0;
    const bool     control    = (flags & (WD_VIDEO_FRAME_END_OF_STREAM | WD_VIDEO_FRAME_RESIZE)) != 0;
    const bool     reset_decoder_before = keyframe && state.session.video_decode_wait_keyframe;
    if (keyframe || control)
    {
        clear_media_queue(state.session.video_decode_queue);
        state.session.video_decode_wait_keyframe = !keyframe;
    }
    else if (state.session.video_decode_wait_keyframe)
    {
        state.stats.video_decode_queue_drops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    else if (state.session.video_decode_queue.size() >= CLIENT_VIDEO_DECODE_QUEUE_CAPACITY)
    {
        clear_media_queue(state.session.video_decode_queue);
        state.session.video_decode_wait_keyframe = true;
        state.stats.video_decode_queue_drops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    state.session.video_decode_queue.push_back(
        ClientMediaPacket{message.message_type, message.payload, message.payload_size, reset_decoder_before});
    message.payload      = nullptr;
    message.payload_size = 0;
    state.session.video_decode_ready.notify_one();
    return true;
}

bool enqueue_audio_message(ClientState& state, wd_tcp_message& message) {
    std::lock_guard<std::mutex> lock(state.session.media_queue_mutex);
    if (!state.session.media_workers_running.load(std::memory_order_acquire))
    {
        return false;
    }

    if (message.message_type == WD_MSG_AUDIO_CONFIG)
    {
        clear_media_queue(state.session.audio_decode_queue);
    }
    else if (state.session.audio_decode_queue.size() >= CLIENT_AUDIO_DECODE_QUEUE_CAPACITY)
    {
        state.stats.audio_decode_queue_drops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    state.session.audio_decode_queue.push_back(
        ClientMediaPacket{message.message_type, message.payload, message.payload_size, false});
    message.payload      = nullptr;
    message.payload_size = 0;
    state.session.audio_decode_ready.notify_one();
    return true;
}

bool handle_video_tcp_message(ClientState& state, wd_tcp_message& message) {
    if (message.message_type != WD_MSG_VIDEO_FRAME)
    {
        return false;
    }

    state.stats.video_messages_rx.fetch_add(1, std::memory_order_relaxed);
    state.stats.video_frames_rx.fetch_add(1, std::memory_order_relaxed);
    state.stats.video_bytes_rx.fetch_add(message.payload_size, std::memory_order_relaxed);
    return enqueue_video_message(state, message);
}

bool handle_audio_tcp_message(ClientState& state, wd_tcp_message& message) {
    if (message.message_type != WD_MSG_AUDIO_CONFIG && message.message_type != WD_MSG_AUDIO_PACKET)
    {
        return false;
    }
    return enqueue_audio_message(state, message);
}

bool handle_selection_tcp_message(ClientState& state, wd_tcp_message& message) {
    if (message.message_type != WD_MSG_CLIPBOARD_SET && message.message_type != WD_MSG_PRIMARY_SET)
    {
        return false;
    }

    return store_selection_text(state, message.payload, message.payload_size, message.message_type == WD_MSG_PRIMARY_SET);
}

bool handle_control_tcp_message(ClientState& state, wd_tcp_message& message) {
    if (message.message_type == WD_MSG_TILE_GENERATION_SUMMARY)
    {
        queue_retransmits_from_summary(state, message.payload, message.payload_size);
        state.stats.tcp_summaries_rx.fetch_add(1, std::memory_order_relaxed);
    }
    else if (message.message_type == WD_MSG_SERVER_CONFIG)
    {
        store_server_config_update(state, message.payload, message.payload_size);
    }
    else if (message.message_type == WD_MSG_CURSOR_SHAPE)
    {
        store_cursor_shape(state, message.payload, message.payload_size);
    }
    else if (message.message_type == WD_MSG_LINK_PROBE_PING)
    {
        return handle_link_probe_ping(state, message.payload, message.payload_size);
    }
    else
    {
        return false;
    }
    return true;
}

enum class ClientTcpDrainResult : uint8_t {
    Healthy,
    PeerClosed,
    Failed,
};

using ClientTcpMessageHandler = bool (*)(ClientState&, wd_tcp_message&);

ClientTcpDrainResult drain_tcp_channel(ClientState& state, int fd, wd_tcp_reader& reader, wd_protocol_channel channel,
                                       ClientTcpMessageHandler handler, uint32_t max_messages) {
    for (uint32_t processed = 0; processed < max_messages; ++processed)
    {
        wd_tcp_message message{};
        const wd_tcp_reader_status status = wd_tcp_reader_receive(&reader, fd, wd_now_ns(), WD_TCP_FRAME_TIMEOUT_NS, &message);
        if (status == WD_TCP_READER_NEED_MORE)
        {
            return ClientTcpDrainResult::Healthy;
        }
        if (status == WD_TCP_READER_PEER_CLOSED)
        {
            return ClientTcpDrainResult::PeerClosed;
        }
        if (status != WD_TCP_READER_MESSAGE)
        {
            WD_LOG_WARN("TCP channel=%u receive failed status=%u", static_cast<unsigned>(channel), static_cast<unsigned>(status));
            return ClientTcpDrainResult::Failed;
        }

        const bool allowed = wd_protocol_message_allowed(message.message_type, channel, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                         WD_PROTOCOL_SERVER_TO_CLIENT, message.payload_size);
        const bool handled = allowed && handler(state, message);
        if (!allowed)
        {
            WD_LOG_WARN("rejected channel=%u message=%s(%u) size=%u", static_cast<unsigned>(channel),
                        wd_protocol_message_name(message.message_type), message.message_type, message.payload_size);
        }
        wd_tcp_message_release(&message);
        if (!handled)
        {
            return ClientTcpDrainResult::Failed;
        }
    }
    return ClientTcpDrainResult::Healthy;
}

void clear_video_decode_queue(ClientState& state);
void clear_audio_decode_queue(ClientState& state);

void close_video_receive_channel(ClientState& state, int fd) {
    clear_video_decode_queue(state);
    reset_video_decoder(state, "video TCP channel closed", WD_CLIENT_VIDEO_PHASE_TILES);
    discard_pending_video_frame(state);
    {
        std::lock_guard<std::mutex> lock(state.session.video_tcp_mutex);
        state.session.video_tcp_connected.store(false, std::memory_order_release);
        state.session.video_unavailable.store(true, std::memory_order_release);
        if (state.session.transport.video_fd == fd)
        {
            state.session.transport.video_fd = -1;
        }
    }
    if (fd >= 0)
    {
        ::close(fd);
    }
    WD_LOG_INFO("video TCP channel closed");
}

void close_audio_receive_channel(ClientState& state, int fd) {
    clear_audio_decode_queue(state);
    client_audio_playback_reset(state.session.audio_playback);
    {
        std::lock_guard<std::mutex> lock(state.session.audio_tcp_mutex);
        state.session.audio_tcp_connected.store(false, std::memory_order_release);
        if (state.session.transport.audio_fd == fd)
        {
            state.session.transport.audio_fd = -1;
        }
    }
    if (fd >= 0)
    {
        ::close(fd);
    }
    WD_LOG_INFO("audio TCP channel closed");
}

void include_reader_deadline(const wd_tcp_reader& reader, uint64_t& deadline_ns) {
    if (!wd_tcp_reader_has_partial_frame(&reader))
    {
        return;
    }
    const uint64_t reader_deadline = wd_tcp_reader_deadline_ns(&reader);
    if (reader_deadline != 0 && reader_deadline < deadline_ns)
    {
        deadline_ns = reader_deadline;
    }
}

int poll_timeout_ms(uint64_t now_ns, uint64_t deadline_ns) {
    if (deadline_ns <= now_ns)
    {
        return 0;
    }
    const uint64_t delta_ns = deadline_ns - now_ns;
    const uint64_t delta_ms = (delta_ns + WD_NSEC_PER_MSEC - 1u) / WD_NSEC_PER_MSEC;
    return delta_ms > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(delta_ms);
}

bool pop_media_packet(ClientState& state, std::deque<ClientMediaPacket>& queue, std::condition_variable& ready,
                      ClientMediaPacket& out) {
    std::unique_lock<std::mutex> lock(state.session.media_queue_mutex);
    ready.wait(lock, [&state, &queue]() {
        return !state.session.media_workers_running.load(std::memory_order_acquire) || !queue.empty();
    });
    if (!state.session.media_workers_running.load(std::memory_order_acquire))
    {
        return false;
    }
    out = queue.front();
    queue.pop_front();
    return true;
}

void client_video_decode_worker_main(ClientState* state) {
    if (!state)
    {
        return;
    }

    ClientMediaPacket packet{};
    while (pop_media_packet(*state, state->session.video_decode_queue, state->session.video_decode_ready, packet))
    {
        if (packet.reset_video_decoder_before)
        {
            reset_video_decoder(*state, "video decode queue overflow");
        }
        handle_video_frame(*state, packet.payload, packet.payload_size);
        release_media_packet(packet);
    }
}

void client_audio_decode_worker_main(ClientState* state) {
    if (!state)
    {
        return;
    }

    ClientMediaPacket packet{};
    while (pop_media_packet(*state, state->session.audio_decode_queue, state->session.audio_decode_ready, packet))
    {
        const bool ok = process_audio_message(*state, packet.message_type, packet.payload, packet.payload_size);
        release_media_packet(packet);
        if (!ok)
        {
            std::lock_guard<std::mutex> lock(state->session.audio_tcp_mutex);
            if (state->session.transport.audio_fd >= 0)
            {
                (void)::shutdown(state->session.transport.audio_fd, SHUT_RDWR);
            }
        }
    }
}

void request_client_media_workers_stop(ClientState& state) {
    {
        std::lock_guard<std::mutex> lock(state.session.media_queue_mutex);
        state.session.media_workers_running.store(false, std::memory_order_release);
        clear_media_queue(state.session.video_decode_queue);
        state.session.video_decode_wait_keyframe = false;
        clear_media_queue(state.session.audio_decode_queue);
    }
    state.session.video_decode_ready.notify_all();
    state.session.audio_decode_ready.notify_all();
}

void stop_client_media_workers(ClientState& state) {
    request_client_media_workers_stop(state);
    if (state.session.video_decode_thread.joinable())
    {
        state.session.video_decode_thread.join();
    }
    if (state.session.audio_decode_thread.joinable())
    {
        state.session.audio_decode_thread.join();
    }
}

bool start_client_media_workers(ClientState& state) {
    if (state.session.media_workers_running.exchange(true, std::memory_order_acq_rel))
    {
        return false;
    }

    try
    {
        if (state.session.transport.video_fd >= 0)
        {
            state.session.video_decode_thread = std::thread(client_video_decode_worker_main, &state);
        }
        if (state.session.transport.audio_fd >= 0)
        {
            state.session.audio_decode_thread = std::thread(client_audio_decode_worker_main, &state);
        }
    }
    catch (...)
    {
        stop_client_media_workers(state);
        return false;
    }
    return true;
}

void clear_video_decode_queue(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.session.media_queue_mutex);
    clear_media_queue(state.session.video_decode_queue);
    state.session.video_decode_wait_keyframe = false;
}

void clear_audio_decode_queue(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.session.media_queue_mutex);
    clear_media_queue(state.session.audio_decode_queue);
}

void client_network_reader_main(ClientState* state) {
    if (!state)
    {
        return;
    }

    int control_fd   = state->session.transport.control_fd;
    int selection_fd = state->session.transport.selection_fd;
    int video_fd     = -1;
    int audio_fd     = -1;
    {
        std::scoped_lock lock(state->session.video_tcp_mutex, state->session.audio_tcp_mutex);
        video_fd = state->session.transport.video_fd;
        audio_fd = state->session.transport.audio_fd;
        state->session.video_tcp_connected.store(video_fd >= 0, std::memory_order_release);
        state->session.audio_tcp_connected.store(audio_fd >= 0, std::memory_order_release);
    }

    wd_tcp_reader control_reader{};
    wd_tcp_reader selection_reader{};
    wd_tcp_reader video_reader{};
    wd_tcp_reader audio_reader{};
    wd_tcp_reader_init(&control_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_CONTROL, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                        WD_PROTOCOL_SERVER_TO_CLIENT));
    wd_tcp_reader_init(&selection_reader,
                       wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_SELECTION, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                       WD_PROTOCOL_SERVER_TO_CLIENT));
    wd_tcp_reader_init(&video_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_VIDEO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                      WD_PROTOCOL_SERVER_TO_CLIENT));
    wd_tcp_reader_init(&audio_reader, wd_protocol_channel_max_payload(WD_PROTOCOL_CHANNEL_AUDIO, WD_PROTOCOL_PHASE_ESTABLISHED,
                                                                      WD_PROTOCOL_SERVER_TO_CLIENT));

    ClientReceiveState* udp_state = client_receive_state_create(*state);
    if (!udp_state)
    {
        WD_LOG_ERROR("failed to create client receive state");
        state->session.running.store(false, std::memory_order_release);
    }

    while (udp_state && state->session.running.load(std::memory_order_acquire))
    {
        const uint64_t now_ns = wd_now_ns();
        uint64_t deadline_ns  = client_receive_udp_deadline_ns(*state, *udp_state, now_ns);
        include_reader_deadline(control_reader, deadline_ns);
        include_reader_deadline(selection_reader, deadline_ns);
        include_reader_deadline(video_reader, deadline_ns);
        include_reader_deadline(audio_reader, deadline_ns);

        std::array<pollfd, 5> pfds{};
        nfds_t                nfds = 0;
        pfds[nfds++]               = pollfd{control_fd, POLLIN, 0};
        pfds[nfds++]               = pollfd{selection_fd, POLLIN, 0};
        if (video_fd >= 0)
        {
            pfds[nfds++] = pollfd{video_fd, POLLIN, 0};
        }
        if (audio_fd >= 0)
        {
            pfds[nfds++] = pollfd{audio_fd, POLLIN, 0};
        }
        if (!client_receive_udp_paused(*state))
        {
            std::lock_guard<std::mutex> lock(state->udp_processing_mutex);
            const int udp_poll_fd = client_async_udp_receiver_poll_fd(state->session.udp_receiver);
            if (udp_poll_fd >= 0)
            {
                pfds[nfds++] = pollfd{udp_poll_fd, POLLIN, 0};
            }
        }

        const int rc = ::poll(pfds.data(), nfds, poll_timeout_ms(now_ns, deadline_ns));
        if (rc < 0 && errno == EINTR)
        {
            continue;
        }
        if (rc < 0)
        {
            WD_LOG_ERROR("client network poll failed: %s", std::strerror(errno));
            break;
        }

        const ClientTcpDrainResult control_result =
            drain_tcp_channel(*state, control_fd, control_reader, WD_PROTOCOL_CHANNEL_CONTROL, handle_control_tcp_message,
                              WD_CLIENT_TCP_DRAIN_BATCH);
        if (control_result != ClientTcpDrainResult::Healthy)
        {
            break;
        }

        const ClientTcpDrainResult selection_result =
            drain_tcp_channel(*state, selection_fd, selection_reader, WD_PROTOCOL_CHANNEL_SELECTION,
                              handle_selection_tcp_message, WD_CLIENT_TCP_DRAIN_BATCH);
        if (selection_result != ClientTcpDrainResult::Healthy)
        {
            break;
        }

        if (!client_receive_udp_service(*state, *udp_state))
        {
            WD_LOG_ERROR("client UDP receive service failed");
            break;
        }

        if (video_fd >= 0)
        {
            const ClientTcpDrainResult video_result =
                drain_tcp_channel(*state, video_fd, video_reader, WD_PROTOCOL_CHANNEL_VIDEO, handle_video_tcp_message, 1);
            if (video_result != ClientTcpDrainResult::Healthy)
            {
                wd_tcp_reader_reset(&video_reader);
                close_video_receive_channel(*state, video_fd);
                video_fd = -1;
            }
        }

        if (audio_fd >= 0)
        {
            const ClientTcpDrainResult audio_result =
                drain_tcp_channel(*state, audio_fd, audio_reader, WD_PROTOCOL_CHANNEL_AUDIO, handle_audio_tcp_message,
                                  WD_CLIENT_TCP_DRAIN_BATCH);
            if (audio_result != ClientTcpDrainResult::Healthy)
            {
                wd_tcp_reader_reset(&audio_reader);
                close_audio_receive_channel(*state, audio_fd);
                audio_fd = -1;
            }
        }
    }

    client_receive_state_destroy(udp_state);
    wd_tcp_reader_destroy(&control_reader);
    wd_tcp_reader_destroy(&selection_reader);
    wd_tcp_reader_destroy(&video_reader);
    wd_tcp_reader_destroy(&audio_reader);
    if (video_fd >= 0)
    {
        close_video_receive_channel(*state, video_fd);
    }
    if (audio_fd >= 0)
    {
        close_audio_receive_channel(*state, audio_fd);
    }
    request_client_media_workers_stop(*state);
    state->session.running.store(false, std::memory_order_release);
}

} // namespace

void client_promote_deferred_summary_retransmits(ClientState& state) {
    promote_deferred_summary_retransmits_locked(state);
}

bool client_connect(ClientState& state, const char* server_host, uint16_t tcp_port, uint16_t client_udp_port,
                    const ClientStreamConfig& stream_config, uint16_t desired_width, uint16_t desired_height) {
    if (!wd_client_session_begin_connect(&state.session.transport))
    {
        WD_LOG_ERROR("client session is already active or still owns transport descriptors");
        return false;
    }

    client_selection_sync_reset(state.selection_sync);

    state.server_host     = server_host ? server_host : "";
    state.tcp_port        = tcp_port;
    state.client_udp_port = client_udp_port;
    state.stream_config   = stream_config;
    state.desired_width   = desired_width;
    state.desired_height  = desired_height;

    state.pending_cursor_shape.store(WD_CURSOR_SHAPE_DEFAULT, std::memory_order_relaxed);
    state.pending_cursor_shape_dirty.store(true, std::memory_order_release);

    if (!client_video_decoder_create(&state.session.video_decoder))
    {
        WD_LOG_WARN("failed to create video decoder skeleton");
    }
    WD_LOG_INFO("video decoder: backend=%s codecs=0x%x available=%s", client_video_decoder_backend_name(state.session.video_decoder),
                client_video_decoder_supported_codecs(state.session.video_decoder),
                client_video_decoder_available(state.session.video_decoder) ? "yes" : "no");

    if (!state.stream_config.disable_audio && client_audio_playback_available())
    {
        if (!client_audio_playback_create(&state.session.audio_playback))
        {
            WD_LOG_WARN("failed to create audio playback state");
        }
    }
    WD_LOG_INFO("audio playback: requested=%s backend=%s available=%s", state.stream_config.disable_audio ? "no" : "yes",
                client_audio_playback_backend_name(), state.session.audio_playback ? "yes" : "no");

    if (!open_udp_socket(state))
    {
        client_disconnect(state);
        return false;
    }

    if (!open_tcp_socket(state))
    {
        client_disconnect(state);
        return false;
    }

    if (!receive_server_config(state))
    {
        client_disconnect(state);
        return false;
    }
    if (!connect_udp_socket_to_server(state, state.config))
    {
        client_disconnect(state);
        return false;
    }

    state.session.control_tcp_sender = create_client_tcp_sender("control");
    if (!state.session.control_tcp_sender)
    {
        client_disconnect(state);
        return false;
    }

    if (!open_input_tcp_socket(state) || !open_selection_tcp_socket(state))
    {
        WD_LOG_ERROR("failed to establish required input and selection TCP channels");
        client_disconnect(state);
        return false;
    }
    WD_LOG_INFO("required input and selection TCP channels enabled");

    if (open_video_tcp_socket(state))
    {
        WD_LOG_INFO("video TCP channel: enabled");
    }
    else if (state.video_stream_negotiated)
    {
        WD_LOG_INFO("video TCP channel: unavailable");
    }

    if (open_audio_tcp_socket(state))
    {
        WD_LOG_INFO("audio TCP channel: enabled");
    }
    else if (state.audio_stream_negotiated)
    {
        WD_LOG_INFO("audio TCP channel: unavailable");
    }

    state.framebuffer.assign(state.framebuffer_pixels(), 0xff202020u);
    state.received_generation.assign(state.tile_count(), 0);
    state.presented_generation.assign(state.tile_count(), 0);
    state.pending_present_generation.assign(state.tile_count(), 0);
    state.pending_tile_telemetry.assign(state.tile_count(), ClientPendingTileTelemetry{});
    if (!configure_client_dirty_tile_grid(state.pending_dirty_tiles, state.config.width, state.config.height, state.config.tile_width,
                                          state.config.tile_height))
    {
        WD_LOG_ERROR("failed to configure client dirty tile grid during connection");
        client_disconnect(state);
        return false;
    }
    client_reset_content_epoch(state, state.config.content_epoch, WD_CLIENT_CONTENT_OWNER_TILES);

    {
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);
        state.retx_queue.clear();
        state.retx_queued_generation.assign(state.tile_count(), 0);
        state.retx_last_requested_generation.assign(state.tile_count(), 0);
        state.retx_last_request_ns.assign(state.tile_count(), 0);
        state.retx_inflight_generation.assign(state.tile_count(), 0);
        state.retx_inflight_since_ns.assign(state.tile_count(), 0);
        state.retx_summary_pending_generation.assign(state.tile_count(), 0);
        state.retx_summary_pending_since_ns.assign(state.tile_count(), 0);
        state.retx_summary_pending_tiles.clear();
        state.retx_summary_pending_position.assign(state.tile_count(), UINT32_MAX);
        state.retx_summary_due_queue.clear();
        state.retx_summary_pending_count         = 0;
        state.next_summary_promote_ns            = 0;
        state.summary_large_repair_not_before_ns = 0;
        state.summary_repair_loss_signal_until_ns.store(0, std::memory_order_relaxed);
    }

    state.udp_recv_buffer.assign(WD_UDP_TILE_HEADER_MAX_SIZE + state.config.udp_payload_target + WD_CLIENT_UDP_RECV_SLACK_BYTES, 0);
    state.session.udp_receiver = create_client_udp_receiver(state, state.config);
    if (!state.session.udp_receiver || !client_async_udp_receiver_ready(state.session.udp_receiver))
    {
        client_disconnect(state);
        return false;
    }

    wd_client_session_mark_connected(&state.session.transport);
    state.session.running.store(true, std::memory_order_release);

    WD_LOG_INFO("connected: session=%u display=%ux%u tiles=%ux%u total=%u", state.config.session_id, state.config.width,
                state.config.height, state.config.tiles_x, state.config.tiles_y, state.config.total_tiles);

    return true;
}

void client_disconnect(ClientState& state) {
    state.session.running.store(false, std::memory_order_release);
    (void)wd_client_session_begin_shutdown(&state.session.transport);

    {
        std::scoped_lock lock(state.session.video_tcp_mutex, state.session.audio_tcp_mutex);
        wd_client_session_shutdown_open_fds(&state.session.transport);
        state.session.video_tcp_connected.store(false, std::memory_order_release);
        state.session.audio_tcp_connected.store(false, std::memory_order_release);
    }

    if (state.session.network_thread.joinable())
    {
        state.session.network_thread.join();
    }
    stop_client_media_workers(state);

    client_reap_async_sends(state);
    destroy_client_tcp_sender(state.session.input_tcp_sender);
    destroy_client_tcp_sender(state.session.selection_tcp_sender);
    destroy_client_tcp_sender(state.session.control_tcp_sender);
    destroy_client_udp_receiver(state);
    {
        std::lock_guard<std::mutex> lock(state.session.video_decoder_mutex);
        client_video_decoder_reset(state.session.video_decoder);
        client_video_decoder_destroy(state.session.video_decoder);
        state.session.video_decoder                = nullptr;
        state.session.video_decoder_needs_keyframe = true;
    }
    client_audio_playback_destroy(state.session.audio_playback);
    state.session.audio_playback = nullptr;

    wd_client_session_close_open_fds(&state.session.transport);
}

void client_reap_async_sends(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.session.async_tcp_stats_mutex);
    const bool healthy = update_async_seen(state, state.session.control_tcp_sender, state.session.control_tcp_seen) &&
                         update_async_seen(state, state.session.input_tcp_sender, state.session.input_tcp_seen) &&
                         update_async_seen(state, state.session.selection_tcp_sender, state.session.selection_tcp_seen);
    if (!healthy)
    {
        WD_LOG_ERROR("client io_uring TCP sender failed; reconnecting the session");
        state.session.running.store(false, std::memory_order_release);
    }
}

void client_reap_async_udp_receives(ClientState& state) {
    update_async_udp_seen(state);
}

bool client_reconfigure_udp_transport_locked(ClientState& state, const wd_server_config_payload& config) {
    const ClientAsyncUdpDetachResult detach_result = destroy_client_udp_receiver(state);
    if (detach_result != ClientAsyncUdpDetachResult::Detached)
    {
        WD_LOG_ERROR("cannot reconfigure UDP transport while io_uring still owns the old socket");
        return false;
    }

    if (state.session.transport.udp_fd >= 0)
    {
        ::close(state.session.transport.udp_fd);
        state.session.transport.udp_fd = -1;
    }
    state.session.udp_seen = ClientAsyncUdpStatsSeen{};

    if (!open_udp_socket(state) || !connect_udp_socket_to_server(state, config))
    {
        if (state.session.transport.udp_fd >= 0)
        {
            ::close(state.session.transport.udp_fd);
            state.session.transport.udp_fd = -1;
        }
        return false;
    }

    state.udp_recv_buffer.assign(WD_UDP_TILE_HEADER_MAX_SIZE + config.udp_payload_target + WD_CLIENT_UDP_RECV_SLACK_BYTES, 0);
    state.session.udp_receiver = create_client_udp_receiver(state, config);
    if (!state.session.udp_receiver || !client_async_udp_receiver_ready(state.session.udp_receiver))
    {
        WD_LOG_ERROR("replacement UDP io_uring receiver initialization failed");
        return false;
    }
    return true;
}

bool client_start_network_worker(ClientState& state) {
    if (state.session.transport.phase != WD_CLIENT_SESSION_CONNECTED || state.session.transport.control_fd < 0 ||
        !state.session.udp_receiver || state.session.network_thread.joinable())
    {
        return false;
    }

    if (!start_client_media_workers(state))
    {
        WD_LOG_ERROR("failed to start client media decode workers");
        return false;
    }

    try
    {
        state.session.network_thread = std::thread(client_network_reader_main, &state);
    }
    catch (...)
    {
        stop_client_media_workers(state);
        return false;
    }
    return true;
}

bool client_send_keyboard_key(ClientState& state, uint16_t evdev_key_code, bool pressed) {
    const int fd = state.session.transport.input_fd;
    if (fd < 0 || evdev_key_code == 0)
    {
        return false;
    }

    wd_keyboard_event_payload event{};
    event.session_id          = state.config.session_id;
    event.connection_token    = state.config.connection_token;
    event.client_timestamp_ns = wd_now_ns();
    event.input_sequence      = next_input_sequence(state);
    event.evdev_key_code      = evdev_key_code;
    event.pressed             = pressed ? 1 : 0;

    const bool ok = client_send_tcp_message_queued(state, fd, WD_MSG_KEYBOARD_KEY, &event, sizeof(event));

    if (ok)
    {
        state.stats.tcp_keyboard_tx.fetch_add(1, std::memory_order_relaxed);
        state.stats.tcp_input_events_tx.fetch_add(1, std::memory_order_relaxed);
        state.stats.tcp_input_channel_tx.fetch_add(1, std::memory_order_relaxed);
        remember_input_timestamp(state, event.input_sequence, event.client_timestamp_ns);
        state.stats.latest_input_event_timestamp_ns.store(event.client_timestamp_ns, std::memory_order_relaxed);
    }

    if (!ok)
    {
        state.session.running.store(false, std::memory_order_release);
    }
    return ok;
}

bool client_send_pointer_event(ClientState& state, const wd_pointer_event_payload& event) {
    wd_pointer_event_payload outbound = event;
    if (outbound.client_timestamp_ns == 0)
    {
        outbound.client_timestamp_ns = wd_now_ns();
    }
    outbound.input_sequence = next_input_sequence(state);

    const int fd = state.session.transport.input_fd;
    if (fd < 0)
    {
        return false;
    }

    const bool ok = client_send_tcp_message_queued(state, fd, WD_MSG_POINTER_EVENT, &outbound, sizeof(outbound));

    if (ok)
    {
        state.stats.tcp_pointer_tx.fetch_add(1, std::memory_order_relaxed);
        state.stats.tcp_input_events_tx.fetch_add(1, std::memory_order_relaxed);
        state.stats.tcp_input_channel_tx.fetch_add(1, std::memory_order_relaxed);

        remember_input_timestamp(state, outbound.input_sequence, outbound.client_timestamp_ns);
        if (outbound.client_timestamp_ns != 0)
        {
            state.stats.latest_input_event_timestamp_ns.store(outbound.client_timestamp_ns, std::memory_order_relaxed);
        }
    }

    if (!ok)
    {
        state.session.running.store(false, std::memory_order_release);
    }
    return ok;
}

bool client_send_selection_text(ClientState& state, uint16_t message_type, const char* text) {
    const int fd = state.session.transport.selection_fd;
    if (fd < 0 || !text)
    {
        return false;
    }

    const size_t text_len = std::strlen(text);
    if (text_len > WD_SELECTION_MAX_TEXT_BYTES)
    {
        WD_LOG_ERROR("selection text too large: %zu bytes, max %u", text_len, WD_SELECTION_MAX_TEXT_BYTES);
        return false;
    }

    std::vector<uint8_t> payload(sizeof(wd_selection_payload_header) + text_len);
    uint32_t             payload_size = 0;
    if (!wd_selection_payload_encode(state.config.session_id, state.config.connection_token, WD_SELECTION_MIME_TEXT_UTF8,
                                     reinterpret_cast<const uint8_t*>(text), static_cast<uint32_t>(text_len), payload.data(),
                                     payload.size(), &payload_size))
    {
        WD_LOG_ERROR("selection text is not valid UTF-8");
        return false;
    }

    const bool ok = client_send_tcp_message_queued(state, fd, message_type, payload.data(), payload_size);
    if (ok)
    {
        state.stats.tcp_selection_channel_tx.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        state.session.running.store(false, std::memory_order_release);
    }
    return ok;
}

static bool client_send_selection_request(ClientState& state, uint16_t message_type) {
    const int fd = state.session.transport.selection_fd;
    if (fd < 0)
    {
        return false;
    }

    const bool ok = client_send_tcp_message_queued(state, fd, message_type, nullptr, 0);
    if (!ok)
    {
        state.session.running.store(false, std::memory_order_release);
    }
    return ok;
}

bool client_send_clipboard_text(ClientState& state, const char* text) {
    return client_send_selection_text(state, WD_MSG_CLIPBOARD_SET, text);
}

bool client_send_primary_text(ClientState& state, const char* text) {
    return client_send_selection_text(state, WD_MSG_PRIMARY_SET, text);
}

bool client_request_server_selections(ClientState& state) {
    const bool clipboard_ok = client_send_selection_request(state, WD_MSG_CLIPBOARD_REQUEST);
    const bool primary_ok   = client_send_selection_request(state, WD_MSG_PRIMARY_REQUEST);
    return clipboard_ok && primary_ok;
}

bool client_send_display_resize(ClientState& state, uint16_t width, uint16_t height) {
    if (state.session.transport.control_fd < 0 || width == 0 || height == 0)
    {
        return false;
    }

    wd_display_resize_payload resize{};
    resize.session_id       = state.config.session_id;
    resize.connection_token = state.config.connection_token;
    resize.width            = width;
    resize.height           = height;

    return client_send_tcp_message_queued(state, state.session.transport.control_fd, WD_MSG_DISPLAY_RESIZE, &resize, sizeof(resize));
}

bool client_send_config_applied(ClientState& state, uint8_t session_id, uint64_t config_epoch) {
    if (state.session.transport.control_fd < 0 || session_id == 0 || config_epoch == 0)
    {
        return false;
    }

    wd_config_applied_payload applied{};
    applied.session_id = session_id;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        applied.connection_token = state.config.connection_token;
    }
    applied.config_epoch = config_epoch;
    return client_send_tcp_message_queued(state, state.session.transport.control_fd, WD_MSG_CONFIG_APPLIED, &applied, sizeof(applied));
}

bool client_send_stats(ClientState& state, const wd_client_stats_payload& stats) {
    if (state.session.transport.control_fd < 0)
    {
        return false;
    }

    return client_send_tcp_message_queued(state, state.session.transport.control_fd, WD_MSG_CLIENT_STATS, &stats, sizeof(stats));
}

bool client_flush_retransmit_requests(ClientState& state) {
    if (state.session.transport.control_fd < 0)
    {
        return false;
    }

    std::vector<wd_tile_repair_entry> entries;
    entries.reserve(MAX_RETRANSMIT_ENTRIES_PER_MESSAGE);

    uint8_t  session_id       = 0;
    uint64_t connection_token = 0;

    {
        std::scoped_lock config_generation_retx_lock(state.config_mutex, state.generation_mutex, state.retx_mutex);

        session_id       = state.config.session_id;
        connection_token = state.config.connection_token;

        const uint64_t now_ns = wd_now_ns();

        const size_t max_entries_this_flush = MAX_RETRANSMIT_ENTRIES_PER_MESSAGE;

        ensure_retransmit_tracking_locked(state, state.config.total_tiles);

        size_t pending_to_scan = state.retx_queue.size();
        while (!state.retx_queue.empty() && pending_to_scan > 0 && entries.size() < max_entries_this_flush)
        {
            pending_to_scan--;
            const uint16_t tile_id = state.retx_queue.front();
            state.retx_queue.pop_front();

            if (tile_id >= state.config.total_tiles || tile_id >= state.retx_queued_generation.size())
            {
                continue;
            }

            const uint64_t target_generation      = state.retx_queued_generation[tile_id];
            state.retx_queued_generation[tile_id] = 0;

            if (target_generation == 0)
            {
                continue;
            }

            /* Tile already arrived while this request was queued. */
            if (state.received_generation[tile_id] >= target_generation)
            {
                continue;
            }

            if (state.retx_inflight_generation[tile_id] >= target_generation && state.retx_inflight_since_ns[tile_id] != 0 &&
                now_ns - state.retx_inflight_since_ns[tile_id] < retransmit_inflight_grace_ns_locked(state))
            {
                state.retx_queued_generation[tile_id] = target_generation;
                state.retx_queue.push_back(tile_id);
                continue;
            }

            if (state.retx_last_requested_generation[tile_id] >= target_generation && state.retx_last_request_ns[tile_id] != 0 &&
                now_ns - state.retx_last_request_ns[tile_id] < retransmit_request_interval_ns(state))
            {
                state.retx_queued_generation[tile_id] = target_generation;
                state.retx_queue.push_back(tile_id);
                continue;
            }

            state.retx_last_requested_generation[tile_id] = target_generation;
            state.retx_last_request_ns[tile_id]           = now_ns;

            wd_tile_repair_entry request{};
            request.tile_id              = tile_id;
            request.requested_generation = target_generation;
            entries.push_back(request);
        }
    }

    if (entries.empty())
    {
        return true;
    }

    wd_tile_repair_request_payload_header header{};
    header.session_id       = session_id;
    header.connection_token = connection_token;
    {
        std::lock_guard<std::mutex> content_lock(state.remote_content_mutex);
        header.content_epoch = state.remote_content_epoch;
    }
    header.request_count = static_cast<uint16_t>(entries.size());

    const size_t payload_size = sizeof(header) + entries.size() * sizeof(wd_tile_repair_entry);

    std::vector<uint8_t> payload(payload_size);

    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), entries.data(), entries.size() * sizeof(wd_tile_repair_entry));

    const bool ok = client_send_tcp_message_queued(state, state.session.transport.control_fd, WD_MSG_TILE_REPAIR_REQUEST, payload.data(),
                                                   static_cast<uint32_t>(payload.size()));

    if (!ok)
    {
        return false;
    }

    state.stats.tcp_retx_requests_tx.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace waydisplay
