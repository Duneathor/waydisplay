#include "present_telemetry.hpp"

#include <algorithm>
#include <unordered_set>

namespace waydisplay {
namespace {

void add_input_sequence(ClientPresentTelemetryBatch& batch, uint64_t sequence) {
    if (sequence == 0)
    {
        return;
    }
    for (uint8_t i = 0; i < batch.input_sequence_count; ++i)
    {
        if (batch.input_sequences[i] == sequence)
        {
            return;
        }
    }
    if (batch.input_sequence_count < batch.input_sequences.size())
    {
        batch.input_sequences[batch.input_sequence_count++] = sequence;
    }
}

} // namespace

void claim_tile_present_telemetry(const std::vector<ClientPendingTileTelemetry>& pending,
                                  const std::vector<ClientTileGenerationUpdate>& updates, uint64_t content_epoch, uint64_t claimed_ns,
                                  ClientPresentTelemetryBatch& out_batch) {
    out_batch.clear();
    out_batch.content_epoch = content_epoch;
    out_batch.claimed_ns    = claimed_ns;

    std::unordered_set<uint64_t> claimed_completion_ids;
    claimed_completion_ids.reserve(updates.size());
    for (const ClientTileGenerationUpdate& update : updates)
    {
        if (update.tile_id >= pending.size())
        {
            continue;
        }
        const ClientPendingTileTelemetry& item = pending[update.tile_id];
        if (item.completion_id == 0 || item.generation != update.generation || item.content_epoch != content_epoch ||
            !claimed_completion_ids.insert(item.completion_id).second)
        {
            continue;
        }

        out_batch.tile_count++;
        if (item.completed_ns != 0)
        {
            out_batch.completion_count++;
            if (claimed_ns >= item.completed_ns)
            {
                out_batch.completion_age_sum_ns += claimed_ns - item.completed_ns;
            }
            if (out_batch.oldest_completion_ns == 0 || item.completed_ns < out_batch.oldest_completion_ns)
            {
                out_batch.oldest_completion_ns = item.completed_ns;
            }
            out_batch.newest_completion_ns = std::max(out_batch.newest_completion_ns, item.completed_ns);
        }
        add_input_sequence(out_batch, item.input_sequence);
    }
}

void commit_tile_present_telemetry(std::vector<ClientPendingTileTelemetry>& pending, const std::vector<ClientTileGenerationUpdate>& updates,
                                   uint64_t content_epoch) {
    for (const ClientTileGenerationUpdate& update : updates)
    {
        if (update.tile_id >= pending.size())
        {
            continue;
        }
        ClientPendingTileTelemetry& item = pending[update.tile_id];
        if (item.content_epoch == content_epoch && item.completion_id != 0 && item.generation == update.generation)
        {
            item = ClientPendingTileTelemetry{};
        }
    }
}

} // namespace waydisplay
