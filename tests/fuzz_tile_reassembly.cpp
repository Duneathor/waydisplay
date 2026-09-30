#include "tile_packet_test_utils.hpp"
#include "tile_reassembly.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace waydisplay;

namespace {

uint8_t fuzz_byte(const uint8_t* data, size_t size, size_t index) {
    return size == 0 ? 0 : data[index % size];
}

void process_and_recycle(TileReassembler& reassembler, ClientState& state,
                         const std::vector<uint8_t>& packet) {
    CompletedTile completed = reassembler.process_udp_packet(state, packet.data(), packet.size());
    if (completed.valid)
    {
        reassembler.recycle_completed_tile_buffer(std::move(completed.tile_bytes));
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    /* Each libFuzzer input remains a complete deterministic test case, but it
     * now describes a sequence of packets rather than one arbitrary datagram.
     * A canonical multi-fragment assembly is always traversed first so entry
     * creation, out-of-order fragments, completion, and recycling stay in the
     * reachable fuzz state space even for a tiny minimized corpus. */
    ClientState state;
    test::initialize_structured_tile_state(state, 1, 2, 1, 256, 256);

    TileReassembler reassembler(8, 8u * 1024u);

    std::array<uint8_t, test::kStructuredFragmentCount> order{};
    for (uint8_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    for (size_t i = order.size(); i > 1; --i)
    {
        const size_t swap_index = fuzz_byte(data, size, i) % i;
        std::swap(order[i - 1], order[swap_index]);
    }

    for (uint8_t fragment_id : order)
    {
        const auto packet = test::make_uncompressed_tile_fragment(
            state, 0, 1, fragment_id, fuzz_byte(data, size, fragment_id),
            fragment_id == 0 ? UINT64_C(1) : 0);
        process_and_recycle(reassembler, state, packet);
    }

    /* Interpret the remaining bytes as stateful operations. Valid packets can
     * replace partial generations or duplicate fragments; selected operations
     * mutate one byte after construction to retain malformed-header coverage. */
    const size_t operation_count = std::min<size_t>(64, (size + 3u) / 4u);
    for (size_t operation = 0; operation < operation_count; ++operation)
    {
        const size_t base = operation * 4u;
        const uint16_t tile_id = static_cast<uint16_t>(
            fuzz_byte(data, size, base) % std::max<uint16_t>(state.config.total_tiles, 1));
        const uint64_t generation = UINT64_C(2) + fuzz_byte(data, size, base + 1u);
        const uint8_t fragment_id = static_cast<uint8_t>(
            fuzz_byte(data, size, base + 2u) % test::kStructuredFragmentCount);
        const uint8_t control = fuzz_byte(data, size, base + 3u);

        auto packet = test::make_uncompressed_tile_fragment(
            state, tile_id, generation, fragment_id, control);
        if ((control & 1u) != 0 && !packet.empty())
        {
            const size_t mutation_index =
                static_cast<size_t>(fuzz_byte(data, size, base + 4u)) % packet.size();
            packet[mutation_index] ^= static_cast<uint8_t>(1u << (control & 7u));
        }
        process_and_recycle(reassembler, state, packet);

        if ((control & 2u) != 0)
        {
            process_and_recycle(reassembler, state, packet);
        }
    }
    return 0;
}
