#include "client_transport.hpp"

#include "client_async_tcp.hpp"
#include "waydisplay/wd_config.h"
#include "waydisplay/wd_log.h"
#include "waydisplay/wd_net.h"
#include "waydisplay/wd_protocol.h"

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace waydisplay {
namespace {

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

    format_socket_endpoint(state.session.transport.udp_fd, false, local, sizeof(local));
    WD_LOG_INFO("UDP receive endpoint local=%s requested_port=%u fd=%d", local, state.client_udp_port, state.session.transport.udp_fd);
}


} // namespace

ClientAsyncTcpSender* create_client_tcp_sender(const char* label) {
    ClientAsyncTcpSender* sender = client_async_tcp_sender_create(WD_CLIENT_TCP_TX_RING_ENTRIES, WD_CLIENT_TCP_TX_PENDING_BYTES);
    if (!sender)
    {
        WD_LOG_ERROR("failed to create io_uring TCP sender for %s channel", label ? label : "unknown");
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

size_t client_async_udp_packet_bytes(const wd_server_config_payload& config) {
    uint16_t udp_payload_target = config.udp_payload_target;
    if (udp_payload_target == 0)
    {
        udp_payload_target = WD_UDP_PAYLOAD_TARGET;
    }
    return WD_UDP_TILE_HEADER_MAX_SIZE + static_cast<size_t>(udp_payload_target) + WD_CLIENT_UDP_RECV_SLACK_BYTES;
}

ClientAsyncUdpReceiver* create_client_udp_receiver(ClientState& state, const wd_server_config_payload& config) {
    const size_t            packet_bytes = client_async_udp_packet_bytes(config);
    ClientAsyncUdpReceiver* receiver     = client_async_udp_receiver_create(state.session.transport.udp_fd, WD_CLIENT_UDP_RX_RING_ENTRIES, packet_bytes);
    if (!receiver)
    {
        WD_LOG_ERROR("failed to create io_uring UDP receiver");
    }
    else if (!client_async_udp_receiver_ready(receiver))
    {
        WD_LOG_ERROR("io_uring UDP receiver setup failed with outstanding receives; reconnect required");
    }
    else
    {
        state.stats.udp_async_receiver_generations.fetch_add(1, std::memory_order_relaxed);
        WD_LOG_INFO("UDP io_uring receive enabled entries=%u buffer=%zu", WD_CLIENT_UDP_RX_RING_ENTRIES, packet_bytes);
    }
    return receiver;
}

void destroy_client_udp_receiver(ClientState& state) {
    if (!state.session.udp_receiver)
    {
        return;
    }

    ClientAsyncUdpReceiverStats   final_stats{};
    const ClientAsyncUdpStatsSeen before = state.session.udp_seen;
    client_async_udp_receiver_destroy(state.session.udp_receiver, &final_stats);
    if (final_stats.posted >= before.posted)
    {
        state.stats.udp_async_posted.fetch_add(final_stats.posted - before.posted, std::memory_order_relaxed);
    }
    if (final_stats.retired >= before.retired)
    {
        const uint64_t drained = final_stats.retired - before.retired;
        state.stats.udp_async_retired.fetch_add(drained, std::memory_order_relaxed);
        state.stats.udp_async_drained_on_reconfigure.fetch_add(drained, std::memory_order_relaxed);
    }
    if (final_stats.completed >= before.completed)
    {
        state.stats.udp_async_completed.fetch_add(final_stats.completed - before.completed, std::memory_order_relaxed);
    }
    if (final_stats.failed >= before.failed)
    {
        state.stats.udp_async_failed.fetch_add(final_stats.failed - before.failed, std::memory_order_relaxed);
    }
    if (final_stats.submit_failed >= before.submit_failed)
    {
        state.stats.udp_async_submit_failed.fetch_add(final_stats.submit_failed - before.submit_failed, std::memory_order_relaxed);
    }
    if (final_stats.cancels >= before.cancels)
    {
        const uint64_t cancelled = final_stats.cancels - before.cancels;
        state.stats.udp_async_cancels.fetch_add(cancelled, std::memory_order_relaxed);
        state.stats.udp_async_cancelled_on_reconfigure.fetch_add(cancelled, std::memory_order_relaxed);
    }
    if (final_stats.accounting_errors >= before.accounting_errors)
    {
        state.stats.udp_async_accounting_errors.fetch_add(final_stats.accounting_errors - before.accounting_errors,
                                                          std::memory_order_relaxed);
    }
    state.stats.udp_async_inflight_current.store(0, std::memory_order_relaxed);
    state.stats.udp_async_prepared_current.store(0, std::memory_order_relaxed);
    state.session.udp_receiver = nullptr;
}

ClientAsyncTcpSender* sender_for_fd(ClientState& state, int fd) {
    if (fd < 0)
    {
        return nullptr;
    }
    if (fd == state.session.transport.input_fd)
    {
        return state.session.input_tcp_sender;
    }
    if (fd == state.session.transport.selection_fd)
    {
        return state.session.selection_tcp_sender;
    }
    if (fd == state.session.transport.control_fd)
    {
        return state.session.control_tcp_sender;
    }
    return nullptr;
}

bool client_send_tcp_message_queued(ClientState& state, int fd, uint16_t message_type, const void* payload, uint32_t payload_size) {
    if (fd < 0)
    {
        return false;
    }

    ClientAsyncTcpSender* sender = sender_for_fd(state, fd);
    return sender && client_async_tcp_send_message(sender, fd, message_type, payload, payload_size);
}

bool update_async_seen(ClientState& state, ClientAsyncTcpSender* sender, ClientAsyncTcpStatsSeen& seen) {
    if (!sender)
    {
        return true;
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
    while (stats.inflight_max > current_max && !state.stats.tcp_async_inflight_max.compare_exchange_weak(
                                                   current_max, stats.inflight_max, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }

    seen.queued            = stats.queued;
    seen.completed         = stats.completed;
    seen.failed            = stats.failed;
    seen.overflows         = stats.overflows;
    seen.partial_resubmits = stats.partial_resubmits;
    seen.coalesced         = stats.coalesced;
    seen.inflight_max      = stats.inflight_max;
    return !stats.fatal;
}

void update_async_udp_seen(ClientState& state) {
    if (!state.session.udp_receiver)
    {
        state.stats.udp_async_inflight_current.store(0, std::memory_order_relaxed);
        state.stats.udp_async_prepared_current.store(0, std::memory_order_relaxed);
        return;
    }

    ClientAsyncUdpReceiverStats stats = client_async_udp_receiver_stats(state.session.udp_receiver);
    ClientAsyncUdpStatsSeen&    seen  = state.session.udp_seen;
    if (stats.posted >= seen.posted)
    {
        state.stats.udp_async_posted.fetch_add(stats.posted - seen.posted, std::memory_order_relaxed);
    }
    if (stats.retired >= seen.retired)
    {
        state.stats.udp_async_retired.fetch_add(stats.retired - seen.retired, std::memory_order_relaxed);
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
    if (stats.accounting_errors >= seen.accounting_errors)
    {
        state.stats.udp_async_accounting_errors.fetch_add(stats.accounting_errors - seen.accounting_errors, std::memory_order_relaxed);
    }

    state.stats.udp_async_inflight_current.store(stats.inflight, std::memory_order_relaxed);
    state.stats.udp_async_prepared_current.store(stats.prepared, std::memory_order_relaxed);

    uint64_t current_max = state.stats.udp_async_inflight_max.load(std::memory_order_relaxed);
    while (stats.inflight_max > current_max && !state.stats.udp_async_inflight_max.compare_exchange_weak(
                                                   current_max, stats.inflight_max, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }

    seen.posted            = stats.posted;
    seen.retired           = stats.retired;
    seen.completed         = stats.completed;
    seen.failed            = stats.failed;
    seen.submit_failed     = stats.submit_failed;
    seen.cancels           = stats.cancels;
    seen.inflight_max      = stats.inflight_max;
    seen.accounting_errors = stats.accounting_errors;
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
    state.session.transport.udp_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (state.session.transport.udp_fd < 0)
    {
        WD_LOG_ERROR("socket UDP failed: %s", std::strerror(errno));
        return false;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons(state.client_udp_port);

    if (::bind(state.session.transport.udp_fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0)
    {
        WD_LOG_ERROR("bind UDP failed: %s", std::strerror(errno));
        ::close(state.session.transport.udp_fd);
        state.session.transport.udp_fd = -1;
        return false;
    }

    set_socket_rcvbuf(state.session.transport.udp_fd, WD_UDP_SOCKET_BUFFER_BYTES);
    log_udp_endpoint(state);

    return true;
}

bool connect_udp_socket_to_server(ClientState& state, const wd_server_config_payload& config) {
    if (state.session.transport.udp_fd < 0 || config.server_udp_port == 0)
    {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(config.server_udp_port);
    if (::inet_pton(AF_INET, state.server_host.c_str(), &addr.sin_addr) != 1)
    {
        WD_LOG_ERROR("invalid IPv4 address for UDP peer: %s", state.server_host.c_str());
        return false;
    }
    if (::connect(state.session.transport.udp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        WD_LOG_ERROR("connect UDP peer failed: %s", std::strerror(errno));
        return false;
    }
    WD_LOG_INFO("UDP peer connected: %s:%u", state.server_host.c_str(), config.server_udp_port);
    return true;
}

int connect_tcp_fd(const ClientState& state, const char* label) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
    state.session.transport.control_fd = connect_tcp_fd(state, "connect TCP");
    if (state.session.transport.control_fd >= 0)
    {
        log_tcp_channel_endpoint("control", state.session.transport.control_fd);
    }
    return state.session.transport.control_fd >= 0;
}

bool open_input_tcp_socket(ClientState& state) {
    int fd = connect_tcp_fd(state, "connect input TCP");
    if (fd < 0)
    {
        return false;
    }

    wd_input_channel_hello_payload hello{};
    hello.session_id       = state.config.session_id;
    hello.connection_token = state.config.connection_token;

    if (!wd_send_tcp_message(fd, WD_MSG_INPUT_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    ClientAsyncTcpSender* sender = create_client_tcp_sender("input");
    if (!sender)
    {
        ::close(fd);
        return false;
    }

    state.session.transport.input_fd = fd;
    state.session.input_tcp_sender   = sender;
    log_tcp_channel_endpoint("input", state.session.transport.input_fd);
    return true;
}

bool open_selection_tcp_socket(ClientState& state) {
    int fd = connect_tcp_fd(state, "connect selection TCP");
    if (fd < 0)
    {
        return false;
    }

    wd_selection_channel_hello_payload hello{};
    hello.session_id       = state.config.session_id;
    hello.connection_token = state.config.connection_token;

    if (!wd_send_tcp_message(fd, WD_MSG_SELECTION_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    ClientAsyncTcpSender* sender = create_client_tcp_sender("selection");
    if (!sender)
    {
        ::close(fd);
        return false;
    }

    state.session.transport.selection_fd = fd;
    state.session.selection_tcp_sender   = sender;
    log_tcp_channel_endpoint("selection", state.session.transport.selection_fd);
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
    hello.session_id       = state.config.session_id;
    hello.connection_token = state.config.connection_token;
    hello.video_codecs     = state.video_codecs;
    hello.video_transport  = state.video_transport;

    if (!wd_send_tcp_message(fd, WD_MSG_VIDEO_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state.session.video_tcp_mutex);
        state.session.transport.video_fd = fd;
        state.session.video_tcp_connected.store(true, std::memory_order_release);
        state.session.video_unavailable.store(false, std::memory_order_release);
    }
    log_tcp_channel_endpoint("video", fd);
    return true;
}

bool open_audio_tcp_socket(ClientState& state) {
    if (!state.audio_stream_negotiated || !state.session.audio_playback)
    {
        return false;
    }

    int fd = connect_tcp_fd(state, "connect audio TCP");
    if (fd < 0)
    {
        return false;
    }

    wd_audio_channel_hello_payload hello{};
    hello.session_id       = state.config.session_id;
    hello.connection_token = state.config.connection_token;
    hello.audio_codecs     = state.audio_codec;
    hello.audio_transport  = state.audio_transport;

    if (!wd_send_tcp_message(fd, WD_MSG_AUDIO_CHANNEL_HELLO, &hello, sizeof(hello)))
    {
        ::close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state.session.audio_tcp_mutex);
        state.session.transport.audio_fd = fd;
        state.session.audio_tcp_connected.store(true, std::memory_order_release);
    }
    log_tcp_channel_endpoint("audio", fd);
    return true;
}


} // namespace waydisplay
