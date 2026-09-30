#include "tile_packet_test_utils.hpp"
#include "tile_reassembly.hpp"
#include "waydisplay/wd_protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <utility>
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

void consume_packet(TileReassembler& reassembler, ClientState& state,
                    const std::vector<uint8_t>& packet, size_t& valid_headers,
                    size_t& active_entries_seen, size_t& completions) {
    wd_udp_tile_packet_decoded decoded{};
    const bool valid = wd_udp_tile_packet_decode(packet.data(), packet.size(), &decoded);
    if (valid)
    {
        valid_headers++;
        require(decoded.header_size <= packet.size(), "decoded header must fit packet");
        require(static_cast<size_t>(decoded.payload_size) + decoded.header_size == packet.size(),
                "valid packets are canonical");
    }

    CompletedTile completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
    active_entries_seen = std::max(active_entries_seen, reassembler.active_entry_count());
    if (completed.valid)
    {
        require(completed.tile_bytes.size() <= WD_TCP_MAX_PAYLOAD_SIZE,
                "fuzz completion must remain bounded");
        completions++;
        reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));
    }
    require(reassembler.active_entry_count() <= 32, "fuzz reassembly entry budget");
    require(reassembler.active_payload_bytes() <= 256u * 1024u,
            "fuzz reassembly byte budget");
}

void test_random_protocol_inputs_are_stateful_and_bounded() {
    std::mt19937_64 random(0x5eed1234ull);
    ClientState state;
    test::initialize_structured_tile_state(state, 7, UINT64_C(0x1111222233334444), 2);
    TileReassembler reassembler(32, 256u * 1024u);

    size_t valid_headers = 0;
    size_t active_entries_seen = 0;
    size_t completions = 0;

    /* Start with a guaranteed valid, out-of-order multi-fragment assembly so
     * the stateful checks below can never pass vacuously because a random
     * byte corpus happened to contain no valid packet. */
    std::array<uint8_t, test::kStructuredFragmentCount> order{};
    for (uint8_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::shuffle(order.begin(), order.end(), random);
    for (uint8_t fragment_id : order)
    {
        const auto packet = test::make_uncompressed_tile_fragment(
            state, 3, 1, fragment_id, 0x31,
            fragment_id == 0 ? UINT64_C(17) : 0);
        require(!packet.empty(), "structured packet construction");
        consume_packet(reassembler, state, packet, valid_headers, active_entries_seen, completions);
    }

    require(valid_headers == test::kStructuredFragmentCount,
            "structured corpus must reach valid packet decoding");
    require(active_entries_seen > 0, "structured corpus must create an active assembly");
    require(completions == 1, "structured corpus must complete a multi-fragment tile");

    /* Exercise state transitions with valid packets mixed with duplicates and
     * malformed mutations. Generation changes replace partial assemblies;
     * duplicate fragments and arbitrary bytes must remain bounded. */
    for (size_t iteration = 0; iteration < 20000; ++iteration)
    {
        if ((iteration % 5u) == 0)
        {
            const uint16_t tile_id = static_cast<uint16_t>(random() % state.config.total_tiles);
            const uint64_t generation = 2u + iteration / 5u;
            const uint8_t fragment_id = static_cast<uint8_t>(random() % test::kStructuredFragmentCount);
            auto packet = test::make_uncompressed_tile_fragment(
                state, tile_id, generation, fragment_id, static_cast<uint8_t>(random()));
            require(!packet.empty(), "valid fuzz packet construction");

            if ((random() & 3u) == 0 && packet.size() > WD_UDP_TILE_HEADER_MIN_SIZE)
            {
                const size_t mutation = static_cast<size_t>(random() % packet.size());
                packet[mutation] ^= static_cast<uint8_t>(1u << (random() & 7u));
            }
            consume_packet(reassembler, state, packet, valid_headers, active_entries_seen, completions);

            if ((random() & 7u) == 0)
            {
                consume_packet(reassembler, state, packet, valid_headers, active_entries_seen, completions);
            }
            continue;
        }

        const size_t size = static_cast<size_t>(random() % 2049u);
        std::vector<uint8_t> bytes(size);
        for (uint8_t& byte : bytes)
        {
            byte = static_cast<uint8_t>(random());
        }
        consume_packet(reassembler, state, bytes, valid_headers, active_entries_seen, completions);
    }

    require(valid_headers > test::kStructuredFragmentCount,
            "stateful fuzz phase must still reach valid protocol packets");
    require(active_entries_seen > 0, "stateful fuzz phase must exercise assembly state");
    require(completions > 0, "stateful fuzz corpus must retain a reachable completion path");
}

} // namespace

int main() {
    test_random_protocol_inputs_are_stateful_and_bounded();
    return 0;
}
