#pragma once

#include "waydisplay/wd_config.h"
#include "waydisplay/wd_protocol.h"
#include "client_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace waydisplay {

enum class TilePacketValidationResult : uint8_t {
    Valid,
    Identity,
    Geometry,
    Fragment,
};

TilePacketValidationResult validate_tile_packet_header(const wd_udp_tile_packet_decoded& header, size_t packet_size,
                                                       uint16_t udp_payload_target, const wd_server_config_payload& config);

struct CompletedTile {
    bool                 valid                  = false;
    uint16_t             tile_id                = 0;
    uint16_t             tile_width             = 0;
    uint16_t             tile_height            = 0;
    uint64_t             generation             = 0;
    uint64_t             content_epoch          = 0;
    uint64_t             input_sequence         = 0;
    uint64_t             first_packet_ns        = 0;
    uint64_t             completed_timestamp_ns = 0;
    uint32_t             compressed_size        = 0;
    uint16_t             packet_count           = 0;
    std::vector<uint8_t> tile_bytes;
};

class TileReassembler {
  public:
    explicit TileReassembler(size_t max_active_entries = WD_CLIENT_TILE_REASSEMBLY_MAX_ACTIVE_ENTRIES,
                             size_t max_active_payload_bytes = WD_CLIENT_TILE_REASSEMBLY_MAX_ACTIVE_PAYLOAD_BYTES);

    void reset();

    CompletedTile process_udp_packet(ClientState& state, const uint8_t* packet, size_t packet_size);

    void     expire_stale_entries(ClientState& state);
    uint64_t next_expiry_deadline_ns(const ClientState& state) const;

    void recycle_completed_tile_buffer(std::vector<uint8_t>&& buffer);

    size_t active_entry_count() const;
    size_t recycled_entry_count() const;
    size_t recycled_completed_buffer_count() const;
    size_t slot_count() const;
    size_t active_payload_bytes() const;

  private:
    struct Entry {
        uint16_t                tile_id                      = 0;
        uint8_t                 tile_size                    = WD_TILE_16x16;
        uint16_t                tile_width                   = 0;
        uint16_t                tile_height                  = 0;
        uint64_t                generation                   = 0;
        uint64_t                content_epoch                = 0;
        uint64_t                input_sequence               = 0;
        bool                    first_fragment_metadata_seen = false;
        uint16_t                packet_count                 = 0;
        uint32_t                compressed_size              = 0;
        bool                    compressed_payload           = true;
        uint64_t                first_packet_ns              = 0;
        std::vector<uint8_t>    compressed;
        std::array<uint64_t, 4> received_bitmap{};
        uint16_t                received_count = 0;
    };

    static constexpr uint32_t INVALID_ENTRY_INDEX = UINT32_MAX;

    static uint64_t count_missing_packets(const Entry& entry);

    void                 expire_entry(ClientState& state, size_t entry_index);
    void                 evict_entry_for_budget(ClientState& state, size_t entry_index);
    void                 queue_entry_repair(ClientState& state, const Entry& entry);
    bool                 ensure_budget(ClientState& state, size_t payload_size);
    bool                 configure_entry_slots(const wd_server_config_payload& config);
    size_t               find_entry_index(uint8_t tile_size, uint16_t tile_id) const;
    size_t               activate_entry(uint8_t tile_size, uint16_t tile_id);
    void                 remove_entry(size_t entry_index);
    void                 recycle_entry(Entry&& entry);
    std::vector<uint8_t> acquire_completed_tile_buffer(size_t size);

    std::array<std::vector<uint32_t>, 4> entry_slots_by_size_{};
    std::vector<Entry>                   active_entries_;
    std::vector<Entry>                   recycled_entries_;
    std::vector<std::vector<uint8_t>>    recycled_completed_buffers_;
    uint16_t                             entry_frame_width_        = 0;
    uint16_t                             entry_frame_height_       = 0;
    size_t                               active_payload_bytes_     = 0;
    size_t                               max_active_entries_       = WD_CLIENT_TILE_REASSEMBLY_MAX_ACTIVE_ENTRIES;
    size_t                               max_active_payload_bytes_ = WD_CLIENT_TILE_REASSEMBLY_MAX_ACTIVE_PAYLOAD_BYTES;
};

} // namespace waydisplay
