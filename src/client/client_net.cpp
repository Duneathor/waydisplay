#include "client_net.hpp"

#include "client_async_tcp.hpp"
#include "client_async_udp.hpp"
#include "video_decoder.hpp"

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"
#include "waydisplay/wd_time.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace waydisplay {
namespace {

constexpr size_t MAX_RETRANSMIT_REQUEST_PAYLOAD_BYTES = WD_TCP_MAX_PAYLOAD_SIZE;
constexpr size_t MAX_RETRANSMIT_REQUEST_ENTRY_CAP =
    (MAX_RETRANSMIT_REQUEST_PAYLOAD_BYTES - sizeof(wd_retransmit_request_payload_header)) / sizeof(wd_retransmit_entry);
constexpr size_t MAX_RETRANSMIT_ENTRIES_PER_MESSAGE =
    MAX_RETRANSMIT_REQUEST_ENTRY_CAP > UINT16_MAX ? UINT16_MAX : MAX_RETRANSMIT_REQUEST_ENTRY_CAP;
constexpr uint64_t RETRANSMIT_GRACE_MIN_NS            = WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS;
constexpr uint64_t RETRANSMIT_GRACE_DEFAULT_NS        = WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS;
constexpr uint64_t RETRANSMIT_GRACE_MAX_NS            = WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS;
constexpr uint64_t MTU_PROBE_SERVER_STARTUP_DELAY_NS  = 10ull * 1000ull * 1000ull;

constexpr uint32_t CLIENT_ASYNC_TCP_RING_ENTRIES = 64;
constexpr uint64_t CLIENT_ASYNC_TCP_MAX_PENDING_BYTES = 1024ull * 1024ull;
constexpr uint32_t CLIENT_ASYNC_UDP_RING_ENTRIES = 256;
constexpr size_t CLIENT_ASYNC_UDP_PACKET_SLACK = 512u;

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
    return static_cast<uint64_t>(ms) * 1000ull * 1000ull;
}

uint64_t clamp_timer_ns(uint64_t ns, uint64_t min_ns, uint64_t max_ns) {
    return std::max(min_ns, std::min(max_ns, ns));
}

uint64_t udp_gap_pressure_ns(const ClientState& state) {
    return clamp_timer_ns(state.udp_gap_pressure_ns.load(std::memory_order_relaxed),
                          0, WD_LINK_RTT_MAX_NS);
}

uint64_t summary_retransmit_grace_ns(const ClientState& state) {
    uint64_t grace_ns = clamp_timer_ns(state.summary_retransmit_grace_ns.load(std::memory_order_relaxed),
                                       WD_LINK_SUMMARY_GRACE_MIN_NS, WD_LINK_SUMMARY_GRACE_MAX_NS);
    uint64_t gap_ns = udp_gap_pressure_ns(state);
    if (gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        uint64_t gap_grace_ns = gap_ns + 50000000ull;
        grace_ns = std::max(grace_ns, clamp_timer_ns(gap_grace_ns,
                                                     WD_LINK_SUMMARY_GRACE_MIN_NS, WD_LINK_SUMMARY_GRACE_MAX_NS));
    }
    return grace_ns;
}

uint64_t retransmit_rerequest_interval_ns(const ClientState& state) {
    uint64_t rerequest_ns = clamp_timer_ns(state.retransmit_rerequest_interval_ns.load(std::memory_order_relaxed),
                                           WD_LINK_RETRANSMIT_REREQUEST_MIN_NS, WD_LINK_RETRANSMIT_REREQUEST_MAX_NS);
    uint64_t gap_ns = udp_gap_pressure_ns(state);
    if (gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        rerequest_ns = std::max(rerequest_ns, clamp_timer_ns(gap_ns * 2ull,
                                                            WD_LINK_RETRANSMIT_REREQUEST_MIN_NS,
                                                            WD_LINK_RETRANSMIT_REREQUEST_MAX_NS));
    }
    return rerequest_ns;
}

uint64_t retransmit_inflight_grace_ns_locked(const ClientState& state) {
    uint64_t inflight_ns = clamp_timer_ns(state.retx_inflight_grace_ns,
                                          WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS,
                                          WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS);
    uint64_t gap_ns = udp_gap_pressure_ns(state);
    if (gap_ns >= WD_LINK_RUNTIME_GAP_PRESSURE_MIN_NS)
    {
        inflight_ns = std::max(inflight_ns, clamp_timer_ns(gap_ns * 2ull,
                                                          WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS,
                                                          WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS));
    }
    return inflight_ns;
}

uint64_t summary_clean_interval_ns_locked(const ClientState& state) {
    return clamp_timer_ns(ms_to_ns(state.config.clean_summary_interval_ms,
                                   WD_LINK_CLEAN_SUMMARY_INTERVAL_DEFAULT_NS),
                          WD_LINK_CLEAN_SUMMARY_INTERVAL_MIN_NS,
                          WD_LINK_CLEAN_SUMMARY_INTERVAL_MAX_NS);
}

uint64_t large_summary_repair_grace_ns_locked(const ClientState& state) {
    return std::max({summary_retransmit_grace_ns(state),
                     retransmit_rerequest_interval_ns(state),
                     summary_clean_interval_ns_locked(state)});
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
    const bool blocked = min_candidate_age_ns < large_grace_ns ||
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
    const uint64_t summary_grace_ns = clamp_timer_ns(ms_to_ns(config.summary_retransmit_grace_ms,
                                                              WD_LINK_SUMMARY_GRACE_DEFAULT_NS),
                                                     WD_LINK_SUMMARY_GRACE_MIN_NS, WD_LINK_SUMMARY_GRACE_MAX_NS);
    const uint64_t rerequest_ns = clamp_timer_ns(ms_to_ns(config.retransmit_rerequest_ms,
                                                          WD_LINK_RETRANSMIT_REREQUEST_DEFAULT_NS),
                                                 WD_LINK_RETRANSMIT_REREQUEST_MIN_NS,
                                                 WD_LINK_RETRANSMIT_REREQUEST_MAX_NS);
    const uint64_t inflight_ns = clamp_timer_ns(ms_to_ns(config.retransmit_inflight_grace_ms,
                                                         WD_LINK_RETRANSMIT_INFLIGHT_DEFAULT_NS),
                                                WD_LINK_RETRANSMIT_INFLIGHT_MIN_NS,
                                                WD_LINK_RETRANSMIT_INFLIGHT_MAX_NS);
    const uint64_t reassembly_ns = clamp_timer_ns(ms_to_ns(config.tile_reassembly_timeout_ms,
                                                           WD_LINK_TILE_REASSEMBLY_DEFAULT_NS),
                                                  WD_LINK_TILE_REASSEMBLY_MIN_NS, WD_LINK_TILE_REASSEMBLY_MAX_NS);
    const uint64_t reassembly_floor_ns = clamp_timer_ns(std::max<uint64_t>(WD_LINK_TILE_REASSEMBLY_MIN_NS, reassembly_ns / 2),
                                                        WD_LINK_TILE_REASSEMBLY_MIN_NS,
                                                        WD_LINK_TILE_REASSEMBLY_MAX_NS);

    state.summary_retransmit_grace_ns.store(summary_grace_ns, std::memory_order_relaxed);
    state.retransmit_rerequest_interval_ns.store(rerequest_ns, std::memory_order_relaxed);

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
    while (state.recent_input_timestamps.size() > 256)
    {
        state.recent_input_timestamps.pop_front();
    }
}

void format_sockaddr_in(const sockaddr_in& addr, char* buf, size_t buf_size) {
    char ip[INET_ADDRSTRLEN]{};

    if (!buf || buf_size == 0)
    {
        return;
    }

    if (::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr)
    {
        std::snprintf(buf, buf_size, "<invalid>:%u", static_cast<unsigned>(ntohs(addr.sin_port)));
        return;
    }

    std::snprintf(buf, buf_size, "%s:%u", ip, static_cast<unsigned>(ntohs(addr.sin_port)));
}

void format_socket_endpoint(int fd, bool peer, char* buf, size_t buf_size) {
    sockaddr_in addr{};
    socklen_t   addr_len = sizeof(addr);

    if (!buf || buf_size == 0)
    {
        return;
    }

    std::snprintf(buf, buf_size, "unavailable");

    if (fd < 0)
    {
        std::snprintf(buf, buf_size, "closed");
        return;
    }

    if ((peer ? ::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len)
              : ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len)) != 0)
    {
        std::snprintf(buf, buf_size, "unavailable:%s", std::strerror(errno));
        return;
    }

    if (addr.sin_family != AF_INET)
    {
        std::snprintf(buf, buf_size, "non-ipv4");
        return;
    }

    format_sockaddr_in(addr, buf, buf_size);
}

void log_tcp_channel_endpoint(const char* channel, int fd) {
    char local[64]{};
    char remote[64]{};

    format_socket_endpoint(fd, false, local, sizeof(local));
    format_socket_endpoint(fd, true, remote, sizeof(remote));
    WD_LOG_INFO("%s TCP channel connected local=%s remote=%s", channel, local, remote);
}

void log_udp_endpoint(const ClientState& state) {
    char local[64]{};

    format_socket_endpoint(state.udp_fd, false, local, sizeof(local));
    WD_LOG_INFO("UDP receive endpoint local=%s requested_port=%u fd=%d", local, state.client_udp_port, state.udp_fd);
}

ClientAsyncTcpSender* create_client_tcp_sender(const char* label) {
    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(CLIENT_ASYNC_TCP_RING_ENTRIES,
                                                                  CLIENT_ASYNC_TCP_MAX_PENDING_BYTES);
    if (!sender)
    {
        WD_LOG_WARN("io_uring TCP sender unavailable for %s channel; using synchronous sends", label ? label : "unknown");
    }
    return sender;
}

void destroy_client_tcp_sender(ClientAsyncTcpSender*& sender) {
    if (sender)
    {
        client_async_tcp_sender_destroy(sender);
        sender = nullptr;
    }
}

size_t client_async_udp_packet_bytes(const ClientState& state) {
    uint16_t udp_payload_target = state.config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }
    return WD_UDP_TILE_HEADER_MAX_SIZE + static_cast<size_t>(udp_payload_target) + CLIENT_ASYNC_UDP_PACKET_SLACK;
}

ClientAsyncUdpReceiver* create_client_udp_receiver(ClientState& state) {
    const size_t packet_bytes = client_async_udp_packet_bytes(state);
    ClientAsyncUdpReceiver* receiver = client_async_udp_receiver_create(state.udp_fd, CLIENT_ASYNC_UDP_RING_ENTRIES,
                                                                        packet_bytes);
    if (!receiver)
    {
        if (state.udp_fd >= 0 && wd_set_nonblocking(state.udp_fd) < 0)
        {
            WD_LOG_ERROR("restore UDP nonblocking failed: %s", std::strerror(errno));
        }
        WD_LOG_WARN("io_uring UDP receiver unavailable; using synchronous recv fallback");
    }
    else
    {
        WD_LOG_INFO("UDP io_uring receive enabled entries=%u buffer=%zu", CLIENT_ASYNC_UDP_RING_ENTRIES, packet_bytes);
    }
    return receiver;
}

void destroy_client_udp_receiver(ClientState& state) {
    if (state.udp_receiver)
    {
        client_async_udp_receiver_destroy(state.udp_receiver);
        state.udp_receiver = nullptr;
    }
}

ClientAsyncTcpSender* sender_for_fd(ClientState& state, int fd) {
    if (fd < 0)
    {
        return nullptr;
    }
    if (fd == state.input_tcp_fd)
    {
        return state.input_tcp_sender;
    }
    if (fd == state.selection_tcp_fd)
    {
        return state.selection_tcp_sender;
    }
    if (fd == state.tcp_fd)
    {
        return state.control_tcp_sender;
    }
    return nullptr;
}

bool client_send_tcp_message_queued(ClientState& state, int fd, uint16_t message_type, const void* payload,
                                    uint32_t payload_size) {
    if (fd < 0)
    {
        return false;
    }

    if (ClientAsyncTcpSender* sender = sender_for_fd(state, fd))
    {
        return client_async_tcp_send_message(sender, fd, message_type, payload, payload_size);
    }

    return wd_send_tcp_message(fd, message_type, payload, payload_size);
}

void update_async_seen(ClientState& state, ClientAsyncTcpSender* sender, ClientAsyncTcpStatsSeen& seen) {
    if (!sender)
    {
        return;
    }

    ClientAsyncTcpSenderStats stats = client_async_tcp_sender_stats(sender);
    if (stats.queued >= seen.queued)
    {
        state.stats.tcp_async_queued.fetch_add(stats.queued - seen.queued, std::memory_order_relaxed);
    }
    if (stats.completed >= seen.completed)
    {
        state.stats.tcp_async_completed.fetch_add(stats.completed - seen.completed, std::memory_order_relaxed);
    }
    if (stats.failed >= seen.failed)
    {
        state.stats.tcp_async_failed.fetch_add(stats.failed - seen.failed, std::memory_order_relaxed);
    }
    if (stats.overflows >= seen.overflows)
    {
        state.stats.tcp_async_overflow.fetch_add(stats.overflows - seen.overflows, std::memory_order_relaxed);
    }
    if (stats.partial_resubmits >= seen.partial_resubmits)
    {
        state.stats.tcp_async_partial.fetch_add(stats.partial_resubmits - seen.partial_resubmits, std::memory_order_relaxed);
    }
    if (stats.coalesced >= seen.coalesced)
    {
        state.stats.tcp_async_coalesced.fetch_add(stats.coalesced - seen.coalesced, std::memory_order_relaxed);
    }

    uint64_t current_max = state.stats.tcp_async_inflight_max.load(std::memory_order_relaxed);
    while (stats.inflight_max > current_max &&
           !state.stats.tcp_async_inflight_max.compare_exchange_weak(current_max, stats.inflight_max,
                                                                     std::memory_order_relaxed,
                                                                     std::memory_order_relaxed))
    {
    }

    seen.queued            = stats.queued;
    seen.completed         = stats.completed;
    seen.failed            = stats.failed;
    seen.overflows         = stats.overflows;
    seen.partial_resubmits = stats.partial_resubmits;
    seen.coalesced         = stats.coalesced;
    seen.inflight_max      = stats.inflight_max;
}

void update_async_udp_seen(ClientState& state) {
    if (!state.udp_receiver)
    {
        return;
    }

    ClientAsyncUdpReceiverStats stats = client_async_udp_receiver_stats(state.udp_receiver);
    ClientAsyncUdpStatsSeen& seen = state.udp_seen;
    if (stats.posted >= seen.posted)
    {
        state.stats.udp_async_posted.fetch_add(stats.posted - seen.posted, std::memory_order_relaxed);
    }
    if (stats.completed >= seen.completed)
    {
        state.stats.udp_async_completed.fetch_add(stats.completed - seen.completed, std::memory_order_relaxed);
    }
    if (stats.failed >= seen.failed)
    {
        state.stats.udp_async_failed.fetch_add(stats.failed - seen.failed, std::memory_order_relaxed);
    }
    if (stats.submit_failed >= seen.submit_failed)
    {
        state.stats.udp_async_submit_failed.fetch_add(stats.submit_failed - seen.submit_failed, std::memory_order_relaxed);
    }
    if (stats.cancels >= seen.cancels)
    {
        state.stats.udp_async_cancels.fetch_add(stats.cancels - seen.cancels, std::memory_order_relaxed);
    }

    uint64_t current_max = state.stats.udp_async_inflight_max.load(std::memory_order_relaxed);
    while (stats.inflight_max > current_max &&
           !state.stats.udp_async_inflight_max.compare_exchange_weak(current_max, stats.inflight_max,
                                                                     std::memory_order_relaxed,
                                                                     std::memory_order_relaxed))
    {
    }

    seen.posted        = stats.posted;
    seen.completed     = stats.completed;
    seen.failed        = stats.failed;
    seen.submit_failed = stats.submit_failed;
    seen.cancels       = stats.cancels;
    seen.inflight_max  = stats.inflight_max;
}

bool set_socket_rcvbuf(int fd, int requested_bytes) {
    if (fd < 0)
    {
        return false;
    }

    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested_bytes, sizeof(requested_bytes)) != 0)
    {
        WD_LOG_ERROR("setsockopt SO_RCVBUF failed: %s", std::strerror(errno));
        return false;
    }

    int       actual_bytes = 0;
    socklen_t actual_len   = sizeof(actual_bytes);

    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_bytes, &actual_len) == 0)
    {
        WD_LOG_INFO("UDP receive buffer: requested=%d actual=%d", requested_bytes, actual_bytes);
    }

    return true;
}

bool open_udp_socket(ClientState& state) {
    state.udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (state.udp_fd < 0)
    {
        WD_LOG_ERROR("socket UDP failed: %s", std::strerror(errno));
        return false;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons(state.client_udp_port);

    if (::bind(state.udp_fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0)
    {
        WD_LOG_ERROR("bind UDP failed: %s", std::strerror(errno));
        ::close(state.udp_fd);
        state.udp_fd = -1;
        return false;
    }

    if (wd_set_nonblocking(state.udp_fd) < 0)
    {
        WD_LOG_ERROR("set UDP nonblocking failed: %s", std::strerror(errno));
        ::close(state.udp_fd);
        state.udp_fd = -1;
        return false;
    }

    set_socket_rcvbuf(state.udp_fd, WD_UDP_SOCKET_BUFFER_BYTES);
    log_udp_endpoint(state);

    return true;
}

int connect_tcp_fd(const ClientState& state, const char* label) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        WD_LOG_ERROR("%s failed: %s", label ? label : "TCP socket", std::strerror(errno));
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(state.tcp_port);

    if (::inet_pton(AF_INET, state.server_host.c_str(), &addr.sin_addr) != 1)
    {
        WD_LOG_ERROR("invalid IPv4 address: %s", state.server_host.c_str());
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        WD_LOG_ERROR("%s failed: %s", label ? label : "TCP socket", std::strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

bool open_tcp_socket(ClientState& state) {
    state.tcp_fd = connect_tcp_fd(state, "connect TCP");
    if (state.tcp_fd >= 0)
    {
        log_tcp_channel_endpoint("control", state.tcp_fd);
    }
    return state.tcp_fd >= 0;
}

bool open_input_tcp_socket(ClientState& state) {
    if ((state.config.capabilities & WD_SERVER_CAP_INPUT_CHANNEL) == 0)
    {
        return false;
    }

    int fd = connect_tcp_fd(state, "connect input TCP");
    if (fd < 0)
    {
        return false;
    }

    wd_input_channel_hello_payload hello{};
    hello.session_id = state.config.session_id;

    if (!wd_send_tcp_message(fd, WD_MSG_INPUT_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    state.input_tcp_fd = fd;
    state.input_tcp_sender = create_client_tcp_sender("input");
    log_tcp_channel_endpoint("input", state.input_tcp_fd);
    return true;
}

bool open_selection_tcp_socket(ClientState& state) {
    if ((state.config.capabilities & WD_SERVER_CAP_SELECTION_CHANNEL) == 0)
    {
        return false;
    }

    int fd = connect_tcp_fd(state, "connect selection TCP");
    if (fd < 0)
    {
        return false;
    }

    wd_selection_channel_hello_payload hello{};
    hello.session_id = state.config.session_id;

    if (!wd_send_tcp_message(fd, WD_MSG_SELECTION_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    state.selection_tcp_fd = fd;
    state.selection_tcp_sender = create_client_tcp_sender("selection");
    log_tcp_channel_endpoint("selection", state.selection_tcp_fd);
    return true;
}

bool open_video_tcp_socket(ClientState& state) {
    if (!state.video_stream_negotiated)
    {
        return false;
    }

    int fd = connect_tcp_fd(state, "connect video TCP");
    if (fd < 0)
    {
        return false;
    }

    wd_video_channel_hello_payload hello{};
    hello.session_id      = state.config.session_id;
    hello.video_codecs    = state.video_codecs;
    hello.video_transport = state.video_transport;

    if (!wd_send_tcp_message(fd, WD_MSG_VIDEO_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state.video_tcp_mutex);
        state.video_tcp_fd = fd;
        state.video_tcp_connected.store(true, std::memory_order_release);
        state.video_unavailable.store(false, std::memory_order_release);
    }
    log_tcp_channel_endpoint("video", fd);
    return true;
}

bool handle_mtu_probe_start(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (payload_size < sizeof(wd_mtu_probe_start_payload))
    {
        return false;
    }

    wd_mtu_probe_start_payload start{};
    std::memcpy(&start, payload, sizeof(start));

    uint16_t              max_received = 0;
    const uint64_t        start_ns     = wd_now_ns();
    const uint64_t        deadline_ns  = start_ns + 500000000ull;
    std::vector<uint64_t> probe_offsets_ns;
    probe_offsets_ns.reserve(start.probe_count);

    std::vector<uint8_t> recvbuf(WD_UDP_TILE_HEADER_MAX_SIZE + 65535);

    while (wd_now_ns() < deadline_ns)
    {
        ssize_t n = ::recv(state.udp_fd, recvbuf.data(), recvbuf.size(), 0);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(1000);
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

        if (h.session_id != start.session_id || h.tile_pkt_count != start.probe_count || h.tile_pkt_id >= start.probe_count)
        {
            continue;
        }

        if (h.compressed_tile_size != h.payload_size)
        {
            continue;
        }

        if (static_cast<size_t>(n) != (size_t)h.header_size + h.payload_size)
        {
            continue;
        }

        const uint64_t rx_ns = wd_now_ns();
        uint64_t offset_ns  = rx_ns - start_ns;
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

        const double stddev_ns = std::sqrt(variance_ns);
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
    result.max_udp_payload_received = max_received;

    return wd_send_tcp_message(state.tcp_fd, WD_MSG_MTU_PROBE_RESULT, &result, sizeof(result));
}

bool handle_throughput_probe_start(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (payload_size < sizeof(wd_throughput_probe_start_payload))
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

    uint64_t bytes_received = 0;
    uint32_t packets_received = 0;
    const uint64_t start_ns = wd_now_ns();
    const uint64_t deadline_ns = start_ns + (static_cast<uint64_t>(start.duration_ms) + 500ull) * 1000ull * 1000ull;

    std::vector<uint8_t> recvbuf(WD_UDP_TILE_HEADER_MAX_SIZE + 65535);

    while (wd_now_ns() < deadline_ns && (duration_limited || packets_received < start.probe_count))
    {
        ssize_t n = ::recv(state.udp_fd, recvbuf.data(), recvbuf.size(), 0);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(1000);
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

        if (h.session_id != start.session_id || h.tile_pkt_count != start.probe_count || h.tile_pkt_id >= start.probe_count)
        {
            continue;
        }

        if (h.payload_size != start.payload_size || h.compressed_tile_size != h.payload_size)
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
    result.session_id      = start.session_id;
    result.bytes_received = bytes_received;
    result.packets_received = packets_received;
    result.duration_ms = duration_ms;

    return wd_send_tcp_message(state.tcp_fd, WD_MSG_THROUGHPUT_PROBE_RESULT, &result, sizeof(result));
}

bool handle_link_probe_ping(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (!payload || payload_size < sizeof(wd_link_probe_payload))
    {
        return false;
    }

    wd_link_probe_payload pong{};
    std::memcpy(&pong, payload, sizeof(pong));
    return wd_send_tcp_message(state.tcp_fd, WD_MSG_LINK_PROBE_PONG, &pong, sizeof(pong));
}

bool receive_server_config(ClientState& state) {
    wd_client_hello_payload hello{};
    hello.client_udp_port      = state.client_udp_port;
    hello.target_fps                  = state.stream_config.target_fps;
    hello.desired_width               = state.desired_width;
    hello.desired_height              = state.desired_height;
    hello.limited_udp_kib_per_second  = state.stream_config.limited_udp_kib_per_second;
    const bool video_allowed = state.stream_config.video_mode != WD_VIDEO_MODE_OFF;
    const uint32_t supported_video_codecs = client_video_decoder_supported_codecs(state.video_decoder);
    const uint32_t requested_video_codecs = state.stream_config.video_codec_mask &
                                           (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265);
    const uint32_t advertised_video_codecs = video_allowed ? (supported_video_codecs & requested_video_codecs) : 0;
    const bool video_decoder_available = advertised_video_codecs != 0;
    hello.capabilities                = video_decoder_available ? WD_CLIENT_CAP_VIDEO_STREAM : 0;
    hello.video_codecs                = advertised_video_codecs;
    hello.video_transport             = video_decoder_available ? WD_VIDEO_TRANSPORT_TCP : 0;
    hello.video_mode                  = state.stream_config.video_mode;
    hello.video_min_dirty_percent     = state.stream_config.video_min_dirty_percent;
    hello.video_enter_seconds         = state.stream_config.video_enter_seconds;
    hello.video_bitrate_kib_per_second = state.stream_config.video_bitrate_kib_per_second;
    hello.video_exit_dirty_percent      = state.stream_config.video_exit_dirty_percent;
    hello.video_exit_seconds            = state.stream_config.video_exit_seconds;

    WD_LOG_INFO("video mode control: mode=%s codec=%s bitrate_kib=%u min_dirty_pct=%u enter_seconds=%u exit_dirty_pct=%u exit_seconds=%u hwdecode=%s decoder=%s",
                video_mode_name(state.stream_config.video_mode),
                video_codec_name(requested_video_codecs),
                static_cast<unsigned>(state.stream_config.video_bitrate_kib_per_second),
                static_cast<unsigned>(state.stream_config.video_min_dirty_percent),
                static_cast<unsigned>(state.stream_config.video_enter_seconds),
                static_cast<unsigned>(state.stream_config.video_exit_dirty_percent),
                static_cast<unsigned>(state.stream_config.video_exit_seconds),
                video_hwdecode_mode_name(state.stream_config.video_hwdecode_mode),
                video_decoder_available ? "yes" : "no");

    if (!wd_send_tcp_message(state.tcp_fd, WD_MSG_CLIENT_HELLO, &hello, sizeof(hello)))
    {
        WD_LOG_ERROR("failed to send CLIENT_HELLO");
        return false;
    }

    for (;;)
    {
        uint16_t message_type = 0;
        uint8_t* payload      = nullptr;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(state.tcp_fd, &message_type, &payload, &payload_size))
        {
            WD_LOG_ERROR("failed to receive SERVER_CONFIG");
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

        if (message_type == WD_MSG_SERVER_CONFIG && payload_size >= sizeof(wd_server_config_payload))
        {
            std::memcpy(&state.config, payload, sizeof(state.config));
            apply_link_timers_from_config(state, state.config);
            std::free(payload);
            break;
        }

        WD_LOG_ERROR("unexpected TCP message while waiting for SERVER_CONFIG: %u", message_type);
        std::free(payload);
        return false;
    }

    const uint32_t expected_tiles = static_cast<uint32_t>(state.config.tiles_x) * static_cast<uint32_t>(state.config.tiles_y);

    if (state.config.width == 0 || state.config.height == 0 || state.config.tile_width == 0 || state.config.tile_height == 0 ||
        state.config.tiles_x == 0 || state.config.tiles_y == 0 || state.config.total_tiles == 0 ||
        expected_tiles != state.config.total_tiles || state.config.pixel_format != WD_PIXEL_FORMAT_XRGB8888 ||
        state.config.compression_mode != WD_COMPRESSION_ZSTD)
    {
        WD_LOG_ERROR("invalid or unsupported server config: server=%ux%u tiles=%ux%u total=%u pixel=%u compression=%u",
                     state.config.width, state.config.height, state.config.tile_width, state.config.tile_height, state.config.total_tiles,
                     state.config.pixel_format, state.config.compression_mode);
        return false;
    }

    if (state.config.udp_payload_target == 0)
    {
        state.config.udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    state.video_stream_negotiated = (state.config.capabilities & WD_SERVER_CAP_VIDEO_STREAM) != 0 &&
                                    (state.config.video_codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265)) != 0 &&
                                    state.config.video_transport == WD_VIDEO_TRANSPORT_TCP;
    state.video_codecs = state.video_stream_negotiated ? state.config.video_codecs : 0;
    state.video_transport = state.video_stream_negotiated ? state.config.video_transport : 0;

    WD_LOG_INFO("UDP payload target: %u", state.config.udp_payload_target);
    WD_LOG_INFO("video stream negotiation: %s codec=%s transport=%s",
                state.video_stream_negotiated ? "enabled" : "unavailable",
                state.video_stream_negotiated ? video_codec_name(state.video_codecs) : "none",
                state.video_stream_negotiated ? "tcp" : "none");
    WD_LOG_INFO("link timers: rtt=%ums summary_grace=%ums rerequest=%ums inflight=%ums reassembly=%ums summary_delta=%u/%ums",
                state.config.link_rtt_ms, state.config.summary_retransmit_grace_ms,
                state.config.retransmit_rerequest_ms, state.config.retransmit_inflight_grace_ms,
                state.config.tile_reassembly_timeout_ms, state.config.active_summary_interval_ms,
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
    if (reset_summary_pending)
    {
        state.retx_summary_pending_count = 0;
    }
}

void clear_summary_pending_locked(ClientState& state, uint16_t tile_id) {
    if (tile_id >= state.retx_summary_pending_generation.size() || tile_id >= state.retx_summary_pending_since_ns.size())
    {
        return;
    }

    if (state.retx_summary_pending_generation[tile_id] != 0 || state.retx_summary_pending_since_ns[tile_id] != 0)
    {
        if (state.retx_summary_pending_count > 0)
        {
            state.retx_summary_pending_count--;
        }
    }

    state.retx_summary_pending_generation[tile_id] = 0;
    state.retx_summary_pending_since_ns[tile_id] = 0;
}

void set_summary_pending_locked(ClientState& state, uint16_t tile_id, uint64_t generation, uint64_t now_ns) {
    if (generation == 0 || tile_id >= state.retx_summary_pending_generation.size() ||
        tile_id >= state.retx_summary_pending_since_ns.size())
    {
        return;
    }

    if (state.retx_summary_pending_generation[tile_id] == 0 && state.retx_summary_pending_since_ns[tile_id] == 0)
    {
        state.retx_summary_pending_count++;
    }
    state.retx_summary_pending_generation[tile_id] = generation;
    state.retx_summary_pending_since_ns[tile_id] = now_ns;
}

bool client_repair_pressure_high_locked(const ClientState& state, uint16_t total_tiles) {
    if (total_tiles == 0)
    {
        return false;
    }

    const uint64_t pressure_tiles = std::max<uint64_t>(WD_CLIENT_REPAIR_PRESSURE_MAX_QUEUE_TILES,
                                                       (uint64_t)total_tiles * WD_CLIENT_REPAIR_PRESSURE_PERCENT / 100ull);
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

    const size_t needed =
        sizeof(wd_tile_summary_payload_header) + static_cast<size_t>(summary.tile_count) * sizeof(wd_tile_generation_entry);

    if (payload_size < needed)
    {
        state.stats.udp_ignored_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto* entries = reinterpret_cast<const wd_tile_generation_entry*>(payload + sizeof(wd_tile_summary_payload_header));

    {
        std::lock_guard<std::mutex> config_lock(state.config_mutex);
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

        if (summary.session_id != state.config.session_id)
        {
            return;
        }

        const uint16_t total_tiles = state.config.total_tiles;
        if (total_tiles == 0 || state.displayed_generation.size() != total_tiles)
        {
            return;
        }

        ensure_retransmit_tracking_locked(state, total_tiles);

        uint64_t newly_deferred_from_summary = 0;
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

            if (entry.tile_generation <= state.displayed_generation[entry.tile_id])
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
                now_ns - state.retx_last_request_ns[entry.tile_id] < retransmit_rerequest_interval_ns(state))
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
            min_candidate_age_ns = std::min(min_candidate_age_ns, candidate_age_ns);
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

        uint64_t newly_queued_from_summary = 0;
        uint64_t summary_to_retx_local_sum_ns = 0;
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
    std::lock_guard<std::mutex> config_lock(state.config_mutex);
    std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
    std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

    const uint16_t total_tiles = state.config.total_tiles;
    if (total_tiles == 0 || state.displayed_generation.size() != total_tiles)
    {
        return;
    }

    ensure_retransmit_tracking_locked(state, total_tiles);
    if (state.retx_summary_pending_count == 0)
    {
        return;
    }

    state.stats.summary_promote_passes.fetch_add(1, std::memory_order_relaxed);
    state.stats.summary_promote_scanned.fetch_add(total_tiles, std::memory_order_relaxed);

    const uint64_t now_ns = wd_now_ns();
    std::vector<SummaryRepairCandidate> candidates;
    candidates.reserve(state.retx_summary_pending_count);
    uint64_t min_candidate_age_ns = UINT64_MAX;

    for (uint16_t tile_id = 0; tile_id < total_tiles; ++tile_id)
    {
        const uint64_t generation = state.retx_summary_pending_generation[tile_id];
        const uint64_t since_ns   = state.retx_summary_pending_since_ns[tile_id];

        if (generation == 0 || since_ns == 0)
        {
            continue;
        }

        if (state.displayed_generation[tile_id] >= generation)
        {
            clear_summary_pending_locked(state, tile_id);
            state.stats.summary_retx_tiles_stale_dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (now_ns - since_ns < summary_retransmit_grace_ns(state))
        {
            continue;
        }

        if (state.retx_inflight_generation[tile_id] >= generation && state.retx_inflight_since_ns[tile_id] != 0 &&
            now_ns - state.retx_inflight_since_ns[tile_id] < retransmit_inflight_grace_ns_locked(state))
        {
            continue;
        }

        if (state.retx_last_requested_generation[tile_id] >= generation && state.retx_last_request_ns[tile_id] != 0 &&
            now_ns - state.retx_last_request_ns[tile_id] < retransmit_rerequest_interval_ns(state))
        {
            continue;
        }

        const uint64_t candidate_age_ns = now_ns >= since_ns ? now_ns - since_ns : 0;
        min_candidate_age_ns = std::min(min_candidate_age_ns, candidate_age_ns);
        candidates.push_back({tile_id, generation, since_ns});
    }

    if (candidates.empty())
    {
        return;
    }

    state.stats.summary_promote_candidates.fetch_add(candidates.size(), std::memory_order_relaxed);

    const size_t pressure_deferred = limit_repair_candidates_under_pressure_locked(state, total_tiles, candidates);
    if (pressure_deferred != 0)
    {
        state.stats.summary_retx_pressure_dropped.fetch_add(pressure_deferred, std::memory_order_relaxed);
    }

    if (large_summary_repair_batch_locked(state, total_tiles, candidates.size(), min_candidate_age_ns, now_ns, true))
    {
        return;
    }

    uint64_t newly_queued = 0;
    uint64_t summary_to_retx_local_sum_ns = 0;
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

bool selection_payload_to_string(const uint8_t* payload, uint32_t payload_size, uint8_t expected_session_id, std::string& out) {
    if (!payload || payload_size < sizeof(wd_selection_payload_header))
    {
        return false;
    }

    wd_selection_payload_header header{};
    std::memcpy(&header, payload, sizeof(header));

    if (header.session_id != expected_session_id ||
        (header.mime_type != WD_SELECTION_MIME_TEXT_UTF8 && header.mime_type != WD_SELECTION_MIME_TEXT_PLAIN) ||
        header.data_size > WD_SELECTION_MAX_TEXT_BYTES)
    {
        return false;
    }

    const size_t needed = sizeof(header) + static_cast<size_t>(header.data_size);
    if (payload_size < needed)
    {
        return false;
    }

    out.assign(reinterpret_cast<const char*>(payload + sizeof(header)), header.data_size);
    return true;
}

void store_selection_text(ClientState& state, const uint8_t* payload, uint32_t payload_size, bool primary) {
    uint8_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id = state.config.session_id;
    }

    std::string text;
    if (!selection_payload_to_string(payload, payload_size, session_id, text))
    {
        return;
    }

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

void store_cursor_shape(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (!payload || payload_size < sizeof(wd_cursor_shape_payload))
    {
        return;
    }

    wd_cursor_shape_payload cursor{};
    std::memcpy(&cursor, payload, sizeof(cursor));

    uint8_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id = state.config.session_id;
    }

    if (cursor.session_id == session_id && cursor.shape < WD_CURSOR_SHAPE_COUNT)
    {
        state.pending_cursor_shape.store(cursor.shape, std::memory_order_relaxed);
        state.pending_cursor_shape_dirty.store(true, std::memory_order_release);
    }
}


void reset_video_decoder(ClientState& state, const char* reason) {
    std::lock_guard<std::mutex> lock(state.video_decoder_mutex);
    client_video_decoder_reset(state.video_decoder);
    state.video_decoder_needs_keyframe = true;
    state.stats.video_decoder_resets.fetch_add(1, std::memory_order_relaxed);
    state.stats.video_last_frame_id_rx.store(0, std::memory_order_relaxed);
    state.stats.video_last_frame_id_presented.store(0, std::memory_order_relaxed);
    if (reason)
    {
        WD_LOG_INFO("video decoder reset: reason=%s", reason);
    }
}

void store_server_config_update(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    if (!payload || payload_size < sizeof(wd_server_config_payload))
    {
        return;
    }

    wd_server_config_payload config{};
    std::memcpy(&config, payload, sizeof(config));

    const uint32_t expected_tiles = static_cast<uint32_t>(config.tiles_x) * static_cast<uint32_t>(config.tiles_y);

    if (config.session_id == 0 || config.width == 0 || config.height == 0 || config.tile_width == 0 || config.tile_height == 0 ||
        config.tiles_x == 0 || config.tiles_y == 0 || config.total_tiles == 0 || expected_tiles != config.total_tiles ||
        config.pixel_format != WD_PIXEL_FORMAT_XRGB8888 || config.compression_mode != WD_COMPRESSION_ZSTD)
    {
        return;
    }

    if (config.udp_payload_target == 0)
    {
        config.udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }

    apply_link_timers_from_config(state, config);

    const bool new_video_stream_negotiated = (config.capabilities & WD_SERVER_CAP_VIDEO_STREAM) != 0 &&
                                             (config.video_codecs & (WD_VIDEO_CODEC_H264 | WD_VIDEO_CODEC_H265)) != 0 &&
                                             config.video_transport == WD_VIDEO_TRANSPORT_TCP;
    const uint32_t new_video_codecs = new_video_stream_negotiated ? config.video_codecs : 0;
    const uint16_t new_video_transport = new_video_stream_negotiated ? config.video_transport : 0;

    bool reset_video = false;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        const bool have_current_config = state.config.session_id != 0;
        reset_video = have_current_config &&
                      (state.config.session_id != config.session_id || state.config.width != config.width ||
                       state.config.height != config.height || state.video_codecs != new_video_codecs ||
                       state.video_transport != new_video_transport);

        state.video_stream_negotiated = new_video_stream_negotiated;
        state.video_codecs = new_video_codecs;
        state.video_transport = new_video_transport;
        state.pending_config       = config;
        state.pending_config_valid = true;
    }

    if (reset_video)
    {
        reset_video_decoder(state, "server config update");
    }
}


bool video_payload_to_packet(ClientState& state, const uint8_t* payload, uint32_t payload_size,
                             ClientVideoPacket& packet, bool& control_frame) {
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

    uint8_t  session_id = 0;
    uint16_t width      = 0;
    uint16_t height     = 0;
    {
        std::lock_guard<std::mutex> lock(state.config_mutex);
        session_id = state.config.session_id;
        width      = state.config.width;
        height     = state.config.height;
    }

    if (packet.header.session_id != session_id)
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (((packet.header.width != width || packet.header.height != height) ||
         packet.header.coded_width < packet.header.width || packet.header.coded_height < packet.header.height) && !control_frame)
    {
        state.stats.video_invalid_frames_rx.fetch_add(1, std::memory_order_relaxed);
        reset_video_decoder(state, "video frame geometry mismatch");
        return false;
    }

    packet.data = wd_video_frame_payload_data(payload, payload_size);
    return true;
}


bool publish_decoded_video_frame(ClientState& state, ClientVideoDecoder* decoder,
                                 const ClientDecodedVideoFrame& frame) {
    if (!decoder || !frame.pixels || frame.width == 0 || frame.height == 0 ||
        frame.stride_pixels < frame.width)
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

    const size_t expected_pixels = static_cast<size_t>(frame.width) * frame.height;
    {
        std::lock_guard<std::mutex> video_lock(state.video_frame_mutex);
        /* Exchange ownership with the decoder instead of copying the decoded
         * BGRA frame. The decoder receives a previously-used buffer and can
         * reuse it on the next frame. */
        if (!client_video_decoder_swap_output_pixels(decoder, state.video_framebuffer))
        {
            return false;
        }
        if (state.video_framebuffer.size() != expected_pixels)
        {
            /* Restore the decoder-owned buffer before reporting failure. */
            (void)client_video_decoder_swap_output_pixels(decoder, state.video_framebuffer);
            return false;
        }
        state.video_frame_width  = frame.width;
        state.video_frame_height = frame.height;
        state.video_frame_id     = frame.frame_id;
        state.video_frame_pts_usec = frame.pts_usec;
    }

    state.pending_video_frame_dirty.store(true, std::memory_order_release);
    state.frame_dirty.store(true, std::memory_order_release);
    return true;
}

void handle_video_frame(ClientState& state, const uint8_t* payload, uint32_t payload_size) {
    ClientVideoPacket packet{};
    bool control_frame = false;
    if (!video_payload_to_packet(state, payload, payload_size, packet, control_frame))
    {
        return;
    }

    if ((packet.header.flags & WD_VIDEO_FRAME_RESIZE) != 0)
    {
        /* The following SERVER_CONFIG carries the authoritative session and
         * geometry. Avoid a duplicate decoder teardown here; just require the
         * next video payload after the config reset to be a keyframe. */
        {
            std::lock_guard<std::mutex> lock(state.video_decoder_mutex);
            state.video_decoder_needs_keyframe = true;
        }
        if (packet.header.data_size == 0)
        {
            return;
        }
    }
    else if ((packet.header.flags & WD_VIDEO_FRAME_END_OF_STREAM) != 0)
    {
        reset_video_decoder(state, "video end-of-stream");
        state.video_unavailable.store(true, std::memory_order_release);
        if (packet.header.data_size == 0)
        {
            return;
        }
    }

    state.stats.video_data_frames_rx.fetch_add(1, std::memory_order_relaxed);

    const uint64_t last_presented = state.stats.video_last_frame_id_presented.load(std::memory_order_relaxed);
    if (last_presented != 0 && packet.header.frame_id <= last_presented)
    {
        /* A fresh keyframe with a lower ID means the sender restarted its
         * codec/sequence without changing the display session. Recover rather
         * than dropping the restarted stream forever. */
        if ((packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0 &&
            packet.header.frame_id < last_presented)
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
        std::lock_guard<std::mutex> lock(state.video_decoder_mutex);
        if (state.video_decoder_needs_keyframe && (packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) == 0)
        {
            state.stats.video_need_keyframe_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ClientVideoDecoderConfig config{};
        config.session_id   = packet.header.session_id;
        config.width        = packet.header.width;
        config.height       = packet.header.height;
        config.coded_width  = packet.header.coded_width != 0 ? packet.header.coded_width : packet.header.width;
        config.coded_height = packet.header.coded_height != 0 ? packet.header.coded_height : packet.header.height;
        config.target_fps    = state.stream_config.target_fps;
        config.codec         = packet.header.codec;
        config.hwdecode_mode = state.stream_config.video_hwdecode_mode;

        if (!client_video_decoder_configure(state.video_decoder, config))
        {
            state.video_decoder_needs_keyframe = true;
            state.stats.video_decode_failed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ClientDecodedVideoFrame frame{};
        const uint64_t decode_start_ns = wd_now_ns();
        bool decoded = client_video_decoder_decode(state.video_decoder, packet, &frame);
        const uint64_t decode_elapsed_ns = wd_now_ns() - decode_start_ns;
        state.stats.video_decode_sum_ns.fetch_add(decode_elapsed_ns, std::memory_order_relaxed);
        state.stats.video_decode_samples.fetch_add(1, std::memory_order_relaxed);

        if (!decoded && client_video_decoder_hwdecode_failed_auto(state.video_decoder))
        {
            /* Auto hardware decode is best-effort. If the VAAPI backend fails
             * while decoding the access unit that unlocks the stream, rebuild
             * the decoder immediately as software and retry that same keyframe
             * instead of waiting for a later periodic keyframe. */
            client_video_decoder_reset(state.video_decoder);
            if ((packet.header.flags & WD_VIDEO_FRAME_KEYFRAME) != 0 &&
                client_video_decoder_configure(state.video_decoder, config))
            {
                frame = ClientDecodedVideoFrame{};
                const uint64_t retry_start_ns = wd_now_ns();
                decoded = client_video_decoder_decode(state.video_decoder, packet, &frame);
                const uint64_t retry_elapsed_ns = wd_now_ns() - retry_start_ns;
                state.stats.video_decode_sum_ns.fetch_add(retry_elapsed_ns, std::memory_order_relaxed);
                state.stats.video_decode_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (!decoded)
        {
            state.stats.video_decode_failed.fetch_add(1, std::memory_order_relaxed);
            state.video_decoder_needs_keyframe = true;
            return;
        }
        if (!frame.pixels)
        {
            return;
        }

        state.stats.video_frames_decoded.fetch_add(1, std::memory_order_relaxed);
        if (!publish_decoded_video_frame(state, state.video_decoder, frame))
        {
            state.stats.video_publish_failed.fetch_add(1, std::memory_order_relaxed);
            state.video_decoder_needs_keyframe = true;
            return;
        }

        state.video_decoder_needs_keyframe = false;
    }

    /* Actual video presentation is recorded by the SDL render thread after
     * SDL_RenderPresent succeeds. Publishing only makes the decoded frame
     * available for upload. */
}

void video_tcp_reader_main(ClientState* state) {
    int fd = -1;
    if (state)
    {
        std::lock_guard<std::mutex> lock(state->video_tcp_mutex);
        fd = state->video_tcp_fd;
        state->video_tcp_connected.store(fd >= 0, std::memory_order_release);
    }
    while (state && state->running.load(std::memory_order_relaxed))
    {
        uint16_t message_type = 0;
        uint8_t* payload      = nullptr;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(fd, &message_type, &payload, &payload_size))
        {
            break;
        }

        if (message_type == WD_MSG_VIDEO_FRAME)
        {
            state->stats.video_messages_rx.fetch_add(1, std::memory_order_relaxed);
            state->stats.video_frames_rx.fetch_add(1, std::memory_order_relaxed);
            state->stats.video_bytes_rx.fetch_add(payload_size, std::memory_order_relaxed);
            handle_video_frame(*state, payload, payload_size);
        }
        else if (message_type == WD_MSG_ERROR)
        {
            WD_LOG_ERROR("server sent MSG_ERROR on video TCP channel");
        }

        std::free(payload);
    }

    reset_video_decoder(*state, "video TCP channel closed");
    {
        std::lock_guard<std::mutex> lock(state->video_tcp_mutex);
        state->video_tcp_connected.store(false, std::memory_order_release);
        state->video_unavailable.store(true, std::memory_order_release);
        if (state->video_tcp_fd == fd)
        {
            state->video_tcp_fd = -1;
        }
    }
    if (fd >= 0)
    {
        ::close(fd);
    }
    WD_LOG_INFO("video TCP channel closed");
}

void tcp_reader_main(ClientState* state) {
    while (state->running.load(std::memory_order_relaxed))
    {
        uint16_t message_type = 0;
        uint8_t* payload      = nullptr;
        uint32_t payload_size = 0;

        if (!wd_recv_tcp_message(state->tcp_fd, &message_type, &payload, &payload_size))
        {
            break;
        }

        if (message_type == WD_MSG_TILE_GENERATION_SUMMARY)
        {
            queue_retransmits_from_summary(*state, payload, payload_size);
            state->stats.tcp_summaries_rx.fetch_add(1, std::memory_order_relaxed);
        }
        else if (message_type == WD_MSG_SERVER_CONFIG)
        {
            store_server_config_update(*state, payload, payload_size);
        }
        else if (message_type == WD_MSG_CLIPBOARD_SET || message_type == WD_MSG_PRIMARY_SET)
        {
            store_selection_text(*state, payload, payload_size, message_type == WD_MSG_PRIMARY_SET);
        }
        else if (message_type == WD_MSG_CURSOR_SHAPE)
        {
            store_cursor_shape(*state, payload, payload_size);
        }
        else if (message_type == WD_MSG_LINK_PROBE_PING)
        {
            (void)handle_link_probe_ping(*state, payload, payload_size);
        }
        else if (message_type == WD_MSG_ERROR)
        {
            WD_LOG_ERROR("server sent MSG_ERROR");
        }

        std::free(payload);
    }

    state->running.store(false, std::memory_order_relaxed);
}

} // namespace

void client_promote_deferred_summary_retransmits(ClientState& state) {
    promote_deferred_summary_retransmits_locked(state);
}

bool client_connect(ClientState& state, const char* server_host, uint16_t tcp_port, uint16_t client_udp_port,
                    const ClientStreamConfig& stream_config, uint16_t desired_width, uint16_t desired_height) {
    state.server_host     = server_host ? server_host : "";
    state.tcp_port        = tcp_port;
    state.client_udp_port = client_udp_port;
    state.stream_config   = stream_config;
    state.desired_width   = desired_width;
    state.desired_height  = desired_height;

    state.pending_cursor_shape.store(WD_CURSOR_SHAPE_DEFAULT, std::memory_order_relaxed);
    state.pending_cursor_shape_dirty.store(true, std::memory_order_release);

    if (!client_video_decoder_create(&state.video_decoder))
    {
        WD_LOG_WARN("failed to create video decoder skeleton");
    }
    WD_LOG_INFO("video decoder: backend=%s codecs=0x%x available=%s",
                client_video_decoder_backend_name(state.video_decoder),
                client_video_decoder_supported_codecs(state.video_decoder),
                client_video_decoder_available(state.video_decoder) ? "yes" : "no");

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

    state.control_tcp_sender = create_client_tcp_sender("control");

    if (open_input_tcp_socket(state))
    {
            WD_LOG_INFO("input TCP channel: enabled");
    }
    else
    {
            WD_LOG_INFO("input TCP channel: unavailable, using control TCP");
    }

    if (open_selection_tcp_socket(state))
    {
            WD_LOG_INFO("selection TCP channel: enabled");
    }
    else
    {
            WD_LOG_INFO("selection TCP channel: unavailable, using control TCP");
    }

    if (open_video_tcp_socket(state))
    {
            WD_LOG_INFO("video TCP channel: enabled");
    }
    else if (state.video_stream_negotiated)
    {
            WD_LOG_INFO("video TCP channel: unavailable");
    }

    state.framebuffer.assign(state.framebuffer_pixels(), 0xff202020u);
    state.displayed_generation.assign(state.tile_count(), 0);

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
        state.retx_summary_pending_count = 0;
        state.next_summary_promote_ns = 0;
        state.summary_large_repair_not_before_ns = 0;
        state.summary_repair_loss_signal_until_ns.store(0, std::memory_order_relaxed);
    }

    state.udp_recv_buffer.assign(WD_UDP_TILE_HEADER_MAX_SIZE + state.config.udp_payload_target + 512, 0);
    state.udp_receiver = create_client_udp_receiver(state);

    state.running.store(true, std::memory_order_relaxed);

    WD_LOG_INFO("connected: session=%u display=%ux%u tiles=%ux%u total=%u", state.config.session_id, state.config.width,
                state.config.height, state.config.tiles_x, state.config.tiles_y, state.config.total_tiles);

    return true;
}

void client_disconnect(ClientState& state) {
    state.running.store(false, std::memory_order_relaxed);

    if (state.tcp_fd >= 0)
    {
        ::shutdown(state.tcp_fd, SHUT_RDWR);
    }
    if (state.input_tcp_fd >= 0)
    {
        ::shutdown(state.input_tcp_fd, SHUT_RDWR);
    }
    if (state.selection_tcp_fd >= 0)
    {
        ::shutdown(state.selection_tcp_fd, SHUT_RDWR);
    }
    {
        std::lock_guard<std::mutex> lock(state.video_tcp_mutex);
        if (state.video_tcp_fd >= 0)
        {
            ::shutdown(state.video_tcp_fd, SHUT_RDWR);
        }
        state.video_tcp_connected.store(false, std::memory_order_release);
    }

    if (state.tcp_thread.joinable())
    {
        state.tcp_thread.join();
    }
    if (state.video_thread.joinable())
    {
        state.video_thread.join();
    }

    client_reap_async_sends(state);
    destroy_client_tcp_sender(state.input_tcp_sender);
    destroy_client_tcp_sender(state.selection_tcp_sender);
    destroy_client_tcp_sender(state.control_tcp_sender);
    destroy_client_udp_receiver(state);
    {
        std::lock_guard<std::mutex> lock(state.video_decoder_mutex);
        client_video_decoder_reset(state.video_decoder);
        client_video_decoder_destroy(state.video_decoder);
        state.video_decoder = nullptr;
        state.video_decoder_needs_keyframe = true;
    }

    if (state.udp_fd >= 0)
    {
        ::close(state.udp_fd);
        state.udp_fd = -1;
    }

    if (state.tcp_fd >= 0)
    {
        ::close(state.tcp_fd);
        state.tcp_fd = -1;
    }
    if (state.input_tcp_fd >= 0)
    {
        ::close(state.input_tcp_fd);
        state.input_tcp_fd = -1;
    }
    if (state.selection_tcp_fd >= 0)
    {
        ::close(state.selection_tcp_fd);
        state.selection_tcp_fd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(state.video_tcp_mutex);
        if (state.video_tcp_fd >= 0)
        {
            ::close(state.video_tcp_fd);
            state.video_tcp_fd = -1;
        }
    }
}

void client_reap_async_sends(ClientState& state) {
    std::lock_guard<std::mutex> lock(state.async_tcp_stats_mutex);
    update_async_seen(state, state.control_tcp_sender, state.control_tcp_seen);
    update_async_seen(state, state.input_tcp_sender, state.input_tcp_seen);
    update_async_seen(state, state.selection_tcp_sender, state.selection_tcp_seen);
}

void client_reap_async_udp_receives(ClientState& state) {
    update_async_udp_seen(state);
}

bool client_disable_async_udp_receiver(ClientState& state) {
    if (!state.udp_receiver)
    {
        return true;
    }

    update_async_udp_seen(state);
    destroy_client_udp_receiver(state);
    state.udp_seen = ClientAsyncUdpStatsSeen{};

    if (state.udp_fd >= 0 && wd_set_nonblocking(state.udp_fd) < 0)
    {
        WD_LOG_ERROR("restore UDP nonblocking failed: %s", std::strerror(errno));
        return false;
    }

    WD_LOG_WARN("UDP io_uring receive failed; falling back to synchronous recv");
    return true;
}

bool client_start_tcp_reader(ClientState& state) {
    if (state.tcp_fd < 0)
    {
        return false;
    }

    state.tcp_thread = std::thread(tcp_reader_main, &state);
    if (state.video_tcp_fd >= 0)
    {
        state.video_thread = std::thread(video_tcp_reader_main, &state);
    }
    return true;
}

bool client_send_keyboard_key(ClientState& state, uint16_t evdev_key_code, bool pressed) {
    const int fd = state.input_tcp_fd >= 0 ? state.input_tcp_fd : state.tcp_fd;
    if (fd < 0 || evdev_key_code == 0)
    {
        return false;
    }

    wd_keyboard_event_payload event{};
    event.session_id          = state.config.session_id;
    event.client_timestamp_ns = wd_now_ns();
    event.input_sequence      = next_input_sequence(state);
    event.evdev_key_code      = evdev_key_code;
    event.pressed             = pressed ? 1 : 0;

    const bool ok = client_send_tcp_message_queued(state, fd, WD_MSG_KEYBOARD_KEY, &event, sizeof(event));

    if (ok)
    {
        state.stats.tcp_keyboard_tx.fetch_add(1, std::memory_order_relaxed);
        state.stats.tcp_input_events_tx.fetch_add(1, std::memory_order_relaxed);
        if (fd == state.input_tcp_fd)
        {
            state.stats.tcp_input_channel_tx.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.stats.tcp_input_channel_fallback_tx.fetch_add(1, std::memory_order_relaxed);
        }
        remember_input_timestamp(state, event.input_sequence, event.client_timestamp_ns);
        state.stats.latest_input_event_timestamp_ns.store(event.client_timestamp_ns, std::memory_order_relaxed);
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

    const int fd = state.input_tcp_fd >= 0 ? state.input_tcp_fd : state.tcp_fd;
    if (fd < 0)
    {
        return false;
    }

    const bool ok = client_send_tcp_message_queued(state, fd, WD_MSG_POINTER_EVENT, &outbound, sizeof(outbound));

    if (ok)
    {
        state.stats.tcp_pointer_tx.fetch_add(1, std::memory_order_relaxed);
        state.stats.tcp_input_events_tx.fetch_add(1, std::memory_order_relaxed);
        if (fd == state.input_tcp_fd)
        {
            state.stats.tcp_input_channel_tx.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.stats.tcp_input_channel_fallback_tx.fetch_add(1, std::memory_order_relaxed);
        }

        remember_input_timestamp(state, outbound.input_sequence, outbound.client_timestamp_ns);
        if (outbound.client_timestamp_ns != 0)
        {
            state.stats.latest_input_event_timestamp_ns.store(outbound.client_timestamp_ns, std::memory_order_relaxed);
        }
    }

    return ok;
}

bool client_send_selection_text(ClientState& state, uint16_t message_type, const char* text) {
    const int fd = state.selection_tcp_fd >= 0 ? state.selection_tcp_fd : state.tcp_fd;
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

    wd_selection_payload_header header{};
    header.session_id = state.config.session_id;
    header.mime_type  = WD_SELECTION_MIME_TEXT_UTF8;
    header.data_size  = static_cast<uint32_t>(text_len);

    std::vector<uint8_t> payload(sizeof(header) + text_len);
    std::memcpy(payload.data(), &header, sizeof(header));
    if (text_len > 0)
    {
        std::memcpy(payload.data() + sizeof(header), text, text_len);
    }

    bool ok = client_send_tcp_message_queued(state, fd, message_type, payload.data(), static_cast<uint32_t>(payload.size()));
    if (!ok && fd == state.selection_tcp_fd && state.tcp_fd >= 0)
    {
        destroy_client_tcp_sender(state.selection_tcp_sender);
        ::close(state.selection_tcp_fd);
        state.selection_tcp_fd = -1;
        ok = client_send_tcp_message_queued(state, state.tcp_fd, message_type, payload.data(), static_cast<uint32_t>(payload.size()));
    }

    if (ok)
    {
        if (fd == state.selection_tcp_fd)
        {
            state.stats.tcp_selection_channel_tx.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.stats.tcp_selection_channel_fallback_tx.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return ok;
}

bool client_send_clipboard_text(ClientState& state, const char* text) {
    return client_send_selection_text(state, WD_MSG_CLIPBOARD_SET, text);
}

bool client_send_primary_text(ClientState& state, const char* text) {
    return client_send_selection_text(state, WD_MSG_PRIMARY_SET, text);
}

bool client_send_display_resize(ClientState& state, uint16_t width, uint16_t height) {
    if (state.tcp_fd < 0 || width == 0 || height == 0)
    {
        return false;
    }

    wd_display_resize_payload resize{};
    resize.session_id = state.config.session_id;
    resize.width      = width;
    resize.height     = height;

    return client_send_tcp_message_queued(state, state.tcp_fd, WD_MSG_DISPLAY_RESIZE, &resize, sizeof(resize));
}


bool client_send_config_applied(ClientState& state, uint8_t session_id) {
    if (state.tcp_fd < 0 || session_id == 0)
    {
        return false;
    }

    wd_config_applied_payload applied{};
    applied.session_id = session_id;
    return client_send_tcp_message_queued(state, state.tcp_fd, WD_MSG_CONFIG_APPLIED, &applied, sizeof(applied));
}


bool client_send_stats(ClientState& state, const wd_client_stats_payload& stats) {
    if (state.tcp_fd < 0)
    {
        return false;
    }

    return client_send_tcp_message_queued(state, state.tcp_fd, WD_MSG_CLIENT_STATS, &stats, sizeof(stats));
}

bool client_flush_retransmit_requests(ClientState& state) {
    if (state.tcp_fd < 0)
    {
        return false;
    }

    std::vector<wd_retransmit_entry> entries;
    entries.reserve(MAX_RETRANSMIT_ENTRIES_PER_MESSAGE);

    uint8_t session_id = 0;

    {
        std::lock_guard<std::mutex> config_lock(state.config_mutex);
        std::lock_guard<std::mutex> gen_lock(state.generation_mutex);
        std::lock_guard<std::mutex> retx_lock(state.retx_mutex);

        session_id = state.config.session_id;

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

            const uint64_t target_generation = state.retx_queued_generation[tile_id];
            state.retx_queued_generation[tile_id] = 0;

            if (target_generation == 0)
            {
                continue;
            }

            /* Tile already arrived while this request was queued. */
            if (state.displayed_generation[tile_id] >= target_generation)
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
                now_ns - state.retx_last_request_ns[tile_id] < retransmit_rerequest_interval_ns(state))
            {
                state.retx_queued_generation[tile_id] = target_generation;
                state.retx_queue.push_back(tile_id);
                continue;
            }

            state.retx_last_requested_generation[tile_id] = target_generation;
            state.retx_last_request_ns[tile_id]          = now_ns;

            wd_retransmit_entry request{};
            request.tile_id              = tile_id;
            request.requested_generation = target_generation;
            entries.push_back(request);
        }
    }

    if (entries.empty())
    {
        return true;
    }

    wd_retransmit_request_payload_header header{};
    header.session_id    = session_id;
    header.request_count = static_cast<uint16_t>(entries.size());

    const size_t payload_size = sizeof(header) + entries.size() * sizeof(wd_retransmit_entry);

    std::vector<uint8_t> payload(payload_size);

    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), entries.data(), entries.size() * sizeof(wd_retransmit_entry));

    const bool ok = client_send_tcp_message_queued(state, state.tcp_fd, WD_MSG_RETRANSMIT_REQUEST, payload.data(), static_cast<uint32_t>(payload.size()));

    if (!ok)
    {
        return false;
    }

    state.stats.tcp_retx_requests_tx.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace waydisplay
