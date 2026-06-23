#pragma once

#include "render_planning.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace waydisplay {

struct ClientPendingTileTelemetry {
    uint64_t completion_id  = 0;
    uint64_t content_epoch  = 0;
    uint64_t generation     = 0;
    uint64_t completed_ns   = 0;
    uint64_t input_sequence = 0;
};

struct ClientPresentTelemetryBatch {
    uint64_t                content_epoch         = 0;
    uint64_t                completion_count      = 0;
    uint64_t                claimed_ns            = 0;
    uint64_t                completion_age_sum_ns = 0;
    uint64_t                oldest_completion_ns  = 0;
    uint64_t                newest_completion_ns  = 0;
    uint64_t                tile_count            = 0;
    std::array<uint64_t, 8> input_sequences{};
    uint8_t                 input_sequence_count = 0;

    bool empty() const {
        return completion_count == 0 && input_sequence_count == 0;
    }
    void clear() {
        *this = ClientPresentTelemetryBatch{};
    }
};

void claim_tile_present_telemetry(const std::vector<ClientPendingTileTelemetry>& pending,
                                  const std::vector<ClientTileGenerationUpdate>& updates, uint64_t content_epoch, uint64_t claimed_ns,
                                  ClientPresentTelemetryBatch& out_batch);

void commit_tile_present_telemetry(std::vector<ClientPendingTileTelemetry>& pending, const std::vector<ClientTileGenerationUpdate>& updates,
                                   uint64_t content_epoch);

} // namespace waydisplay
