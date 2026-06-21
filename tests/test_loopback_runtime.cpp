#include "tile_reassembly.hpp"

#include "waydisplay/wd_protocol.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace waydisplay;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

void initialize_state(ClientState& state, uint16_t udp_payload_target) {
    state.config.session_id = 9;
    state.config.connection_token = 0x123456789abcdef0ull;
    state.config.content_epoch = 3;
    state.config.config_epoch = 4;
    state.config.server_udp_port = 5001;
    state.config.width = 64;
    state.config.height = 64;
    state.config.tile_width = 16;
    state.config.tile_height = 16;
    state.config.tiles_x = 4;
    state.config.tiles_y = 4;
    state.config.total_tiles = 16;
    state.config.udp_payload_target = udp_payload_target;
    state.received_generation.assign(state.config.total_tiles, 0);
    state.retx_queued_generation.assign(state.config.total_tiles, 0);
    state.retx_last_requested_generation.assign(state.config.total_tiles, 0);
    state.retx_last_request_ns.assign(state.config.total_tiles, 0);
    state.retx_inflight_generation.assign(state.config.total_tiles, 0);
    state.retx_inflight_since_ns.assign(state.config.total_tiles, 0);
}

std::vector<std::vector<uint8_t>> make_packets(const ClientState& state, uint16_t tile_id,
                                               uint64_t generation, const std::vector<uint8_t>& payload) {
    const uint16_t target = state.config.udp_payload_target;
    const uint8_t count = static_cast<uint8_t>((payload.size() + target - 1u) / target);
    require(count >= 2, "loopback test should exercise fragmented tiles");

    std::vector<std::vector<uint8_t>> packets;
    packets.reserve(count);
    for (uint8_t packet_id = 0; packet_id < count; ++packet_id)
    {
        const size_t offset = static_cast<size_t>(packet_id) * target;
        const uint16_t fragment_size = static_cast<uint16_t>(std::min<size_t>(target, payload.size() - offset));
        wd_udp_tile_packet_decoded header{};
        header.session_id = state.config.session_id;
        header.connection_token = state.config.connection_token;
        header.content_epoch = state.config.content_epoch;
        header.flags = packet_id == 0 ? WD_UDP_TILE_FLAG_INPUT_SEQUENCE : 0;
        header.tile_size = WD_TILE_16x16;
        header.tile_pkt_id = packet_id;
        header.tile_id = tile_id;
        header.tile_pkt_count = count;
        header.payload_size = fragment_size;
        header.tile_payload_size = static_cast<uint16_t>(payload.size());
        header.tile_generation = generation;
        header.input_sequence = packet_id == 0 ? 77 : 0;
        const uint16_t header_size = wd_udp_tile_header_size_for_flags(header.flags);
        std::vector<uint8_t> packet(header_size + fragment_size);
        require(wd_udp_tile_packet_encode_header(packet.data(), packet.size(), &header), "encode loopback packet");
        std::memcpy(packet.data() + header_size, payload.data() + offset, fragment_size);
        packets.push_back(std::move(packet));
    }
    return packets;
}

void test_udp_loopback_reorders_and_deduplicates_fragments() {
    int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    require(receiver >= 0 && sender >= 0, "create UDP loopback sockets");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(receiver, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "bind UDP receiver");
    socklen_t address_size = sizeof(address);
    require(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &address_size) == 0, "query UDP receiver");

    ClientState state;
    initialize_state(state, 400);
    std::vector<uint8_t> tile(16u * 16u * WD_BYTES_PER_PIXEL);
    for (size_t i = 0; i < tile.size(); ++i)
    {
        tile[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xffu);
    }
    auto packets = make_packets(state, 5, 11, tile);

    std::vector<size_t> order;
    order.reserve(packets.size() + 1u);
    for (size_t i = packets.size(); i > 0; --i)
    {
        order.push_back(i - 1u);
    }
    order.insert(order.begin() + 1, order.front());

    for (size_t index : order)
    {
        const auto& packet = packets[index];
        require(sendto(sender, packet.data(), packet.size(), 0,
                       reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                    static_cast<ssize_t>(packet.size()),
                "send complete UDP datagram");
    }

    TileReassembler reassembler;
    CompletedTile completed{};
    std::vector<uint8_t> receive_buffer(2048);
    for (size_t i = 0; i < order.size(); ++i)
    {
        const ssize_t size = recv(receiver, receive_buffer.data(), receive_buffer.size(), 0);
        require(size > 0, "receive UDP datagram");
        CompletedTile candidate = reassembler.process_udp_packet(state, receive_buffer.data(), static_cast<size_t>(size));
        if (candidate.valid)
        {
            require(!completed.valid, "one logical tile should complete once despite duplicates");
            completed = std::move(candidate);
        }
    }

    require(completed.valid, "reordered loopback fragments should complete");
    require(completed.tile_id == 5 && completed.generation == 11, "completed tile identity");
    require(completed.input_sequence == 77, "first-fragment metadata should survive reordering");
    require(completed.tile_bytes == tile, "loopback tile payload should round trip");

    close(sender);
    close(receiver);
}

void test_old_connection_packet_is_rejected_after_reset() {
    ClientState state;
    initialize_state(state, 400);
    std::vector<uint8_t> tile(16u * 16u * WD_BYTES_PER_PIXEL, 0x44);
    auto packets = make_packets(state, 0, 5, tile);
    state.config.connection_token++;

    TileReassembler reassembler;
    for (const auto& packet : packets)
    {
        CompletedTile completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
        require(!completed.valid, "old connection packet must not complete after reset");
    }
    require(reassembler.active_entry_count() == 0, "old connection packets must not allocate reassembly state");
}

} // namespace

int main() {
    test_udp_loopback_reorders_and_deduplicates_fragments();
    test_old_connection_packet_is_rejected_after_reset();
    return 0;
}
