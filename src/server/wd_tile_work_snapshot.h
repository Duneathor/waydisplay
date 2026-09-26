#pragma once

#include "waydisplay/wd_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wd_tile_work_snapshot_entry {
    uint16_t base_tile_id;
    uint64_t dirty_epoch;
    bool     selected;
};

struct wd_tile_work_snapshot {
    struct wd_tile_work_snapshot_entry entries[WD_WIRE_TILE_MAX_BASE_TILES];
    uint16_t                           count;
};

void wd_tile_work_snapshot_reset(struct wd_tile_work_snapshot* snapshot);
bool wd_tile_work_snapshot_add(struct wd_tile_work_snapshot* snapshot, uint16_t base_tile_id, bool selected, uint64_t dirty_epoch);
bool wd_tile_work_snapshot_lookup(const struct wd_tile_work_snapshot* snapshot, uint16_t base_tile_id, bool* out_selected,
                                  uint64_t* out_dirty_epoch);
bool wd_tile_work_snapshot_any_selected(const struct wd_tile_work_snapshot* snapshot, const uint16_t* base_tile_ids, uint16_t count);

#ifdef __cplusplus
}
#endif
